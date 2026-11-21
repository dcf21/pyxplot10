// children.c
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

// Manages Pyxplot's child processes. At startup, the main process forks the Child Support Process (CSP), which creates
// Pyxplot's temporary directory and launches postscript viewers on request, keeping them open as the X11 terminals
// require (including after Pyxplot exits, for X11_persist), and removes the directory when everything has finished; the
// two processes talk through a pair of pipes. The main process also launches short-lived helpers itself (sed, latex,
// input filters and kpsewhich), connected to it by pipes, and relays error messages which the CSP and helpers send
// back.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "coreUtils/memAlloc.h"
#include "coreUtils/errorReport.h"

#include "settings/settingTypes.h"

#include "children.h"

// Pipes for communication between the main Pyxplot process and the CSP

int PipeCSP2MAIN[2] = {0, 0};
int PipeMAIN2CSP[2] = {0, 0};

char PipeOutputBuffer[LSTR_LENGTH] = "";

static char SIGTERM_NAME
        [16]; // The name of SIGTERM, which we filter out from GV's output, as it tends to whinge about being killed.

static ppl_context *static_context;

// Flags used to keep track of all of the open GhostView processes

#define PTABLE_LISTLEN 4096

static int *GhostViews; // A list of X11_multiwindow and X11_singlewindow sessions which we kill on Pyxplot exit
static int *
        GhostView_Persists; // A list of X11_persist sessions for which we leave our temporary directory until they quit
static int *HelperPIDs; // A list of helper processes forked by the main Pyxplot process
static int GhostView_pid = 0; // pid of any running gv process launched under X11_singlewindow
static int PyxplotRunning = 1; // Flag which we drop in the CSP when the main process stops running

// Functions to be called from main Pyxplot process

//! pplcsp_init - Fork the Child Support Process (CSP), which runs alongside the main Pyxplot process to manage
//! external viewers such as GhostView, create the temporary working directory, and clean up after Pyxplot exits.
//! The main process returns from this function; the CSP never does.
//! \param [in] context - The Pyxplot context

void pplcsp_init(ppl_context *context) {
    // Variables for creating the CSP
    int pid, fail, i;
    struct stat statinfo;

    // Create empty lists for storing lists of GhostView processes
    GhostViews = (int *) malloc(PTABLE_LISTLEN * sizeof(int));
    GhostView_Persists = (int *) malloc(PTABLE_LISTLEN * sizeof(int));
    HelperPIDs = (int *) malloc(PTABLE_LISTLEN * sizeof(int));
    static_context = context;

    // Failure to allocate memory is fatal
    if ((GhostViews == NULL) || (GhostView_Persists == NULL) || (HelperPIDs == NULL))
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Out of memory.");
    for (i = 0; i < PTABLE_LISTLEN; i++) GhostViews[i] = 0;
    for (i = 0; i < PTABLE_LISTLEN; i++) GhostView_Persists[i] = 0;
    for (i = 0; i < PTABLE_LISTLEN; i++) HelperPIDs[i] = 0;

    // The string "signal 15" we filter out of GhostView's output
    ppl_snprintfTrunc(SIGTERM_NAME, sizeof(SIGTERM_NAME), "signal %d", SIGTERM);

    // Create pipes for communication between main process and the CSP
    if (pipe(PipeCSP2MAIN) < 0) ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not create a pipe.");
    if (pipe(PipeMAIN2CSP) < 0) ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not create a pipe.");

    // Fork the CSP
    if ((pid = fork()) < 0)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not fork a child process for the CSP.");
    // The main process sets its SIGCHLD handler and returns; the rest of this function runs in the CSP
    else if (pid != 0) {
        close(PipeMAIN2CSP[0]); // Parent process; close CSP's ends of pipes
        //close(PipeCSP2MAIN[1]); // Leave this pipe open so that sed can return error messages down it
        if (signal(SIGCHLD, pplcsp_checkForHelperExits) == SIG_ERR)
            ppl_fatal(&context->errcontext, __FILE__, __LINE__,
                      "Main process could not set up a signal handler for SIGCHLD.");
        return; // Parent process returns
    }

    // Close main process's ends of the pipes
    close(PipeCSP2MAIN[0]);
    close(PipeMAIN2CSP[1]);

    // Make all log messages appear to come from the CSP
    ppl_snprintfTrunc(context->errcontext.error_source, sizeof(context->errcontext.error_source), "CSP%6d", getpid());
    signal(SIGINT, SIG_IGN); // Ignore SIGINT
    if (setpgid(getpid(), getpid()) < 0)
        if (DEBUG)
            ppl_log(&context->errcontext,
                    "Failed to set process group ID."); // Make into a process group leader so that we won't catch SIGINT

    // Make temporary working directory
    fail = 0;
    if ((mkdir(context->errcontext.session_default.tempdir, 0700) != 0) || // Create temporary working directory
        (access(context->errcontext.session_default.tempdir, F_OK) != 0)) {
        fail = 1;
    } // If temporary directory does not exist, fail.
    // The directory must be a directory owned by this user
    else {
        if (stat(context->errcontext.session_default.tempdir, &statinfo) < 0)
            fail = 1; // Otherwise stat it and make sure it's a directory we own
        if (!S_ISDIR(statinfo.st_mode)) fail = 1;
        if (statinfo.st_uid != getuid()) fail = 1;
    }
    if (fail == 1) { ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Failed to create temporary directory."); }
    if (chdir(context->errcontext.session_default.tempdir) < 0) {
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "chdir into temporary directory failed.");
    } // chdir into temporary directory

    // Enter CSP execution loop
    if (signal(SIGCHLD, pplcsp_checkForChildExits) == SIG_ERR)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "CSP could not set up a signal handler for SIGCHLD.");
    pplcsp_main(context);
    close(PipeCSP2MAIN[1]); // Close pipes
    close(PipeMAIN2CSP[0]);
    if (chdir("/") < 0)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__,
                  "chdir out of temporary directory failed."); // chdir out of temporary directory
    ppl_snprintfTrunc(PipeOutputBuffer, sizeof(PipeOutputBuffer), "rm -Rf %s",
                      context->errcontext.session_default.tempdir);
    if (system(PipeOutputBuffer) < 0)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__,
                  "Removal of temporary directory failed."); // Remove temporary directory
    ppl_memAlloc_FreeAll(0);
    if (DEBUG) ppl_log(&context->errcontext, "CSP terminating normally.");
    exit(0);
}

//! pplcsp_checkForGvOutput - Relay any error messages which the CSP has passed back to the main process, e.g. from
//! a postscript viewer which failed to launch
//! \param [in] context - The Pyxplot context

void pplcsp_checkForGvOutput(ppl_context *context) {
    struct timespec waitperiod; // A time.h timespec specifier for a wait of zero seconds
    fd_set readable;
    int pos, TrialNumber;
    ssize_t nread;
    char linebuffer[SSTR_LENGTH];

    // Check whether the pipe from the CSP is readable, retrying up to three times if interrupted by a signal
    TrialNumber = 1;
    // Retry if interrupted by a signal
    while (1) {
        waitperiod.tv_sec = waitperiod.tv_nsec = 0;
        FD_ZERO(&readable);
        FD_SET(PipeCSP2MAIN[0], &readable);
        // Report a failure of pselect
        if (pselect(PipeCSP2MAIN[0] + 1, &readable, NULL, NULL, &waitperiod, NULL) == -1) {
            // Retry if interrupted by a signal
            if ((errno == EINTR) && (TrialNumber < 3)) {
                TrialNumber++;
                continue;
            }
            if (DEBUG) ppl_log(&context->errcontext, "pselect failure whilst checking for GV output");
            return;
        }
        break;
    }
    if (!FD_ISSET(PipeCSP2MAIN[0], &readable)) return; // select tells us that pipe from CSP is not readable

    // Read what is waiting, and report it line by line
    pos = strlen(PipeOutputBuffer);
    nread = read(PipeCSP2MAIN[0], PipeOutputBuffer + pos, LSTR_LENGTH - pos - 5);
    // read() does not terminate the string, so terminate it after the new bytes
    if (nread > 0) PipeOutputBuffer[pos + nread] = '\0';
    if (nread > 0)
        // Report each complete line, ignoring messages about viewers being killed
        while (PipeOutputBuffer[0] != '\0') {
            ppl_strRemoveCompleteLine(PipeOutputBuffer, linebuffer);
            if (linebuffer[0] == '\0') continue;
            if (strstr(linebuffer, SIGTERM_NAME) != NULL) continue;
            if (strncmp(linebuffer, SED_COMMAND, strlen(SED_COMMAND)) == 0)
                ppl_error(&context->errcontext, ERR_GENERIC, -1, -1,
                          "A problem was encounter with the supplied regular expression.");
            // Warnings, such as X toolkit font warnings from ghostview, are reported as warnings
            else if (strncmp(linebuffer, "Warning:", 8) == 0) {
                // Messages such as X toolkit font warnings from ghostview are not errors: they should neither be
                // labelled as errors, nor make Pyxplot exit with a non-zero status. ppl_warning() adds its own prefix.
                char *text = linebuffer + 8;
                while (*text == ' ') text++;
                ppl_warning(&context->errcontext, ERR_GENERIC, text);
            } else ppl_error(&context->errcontext, ERR_GENERIC, -1, -1, linebuffer);
        }
    return;
}

//! pplcsp_sendCommand - Send a command to the CSP through its command pipe
//! \param [in] context - The Pyxplot context
//! \param [in] cmd - The command to send. The first character identifies the command; see pplcsp_processCommand().

void pplcsp_sendCommand(ppl_context *context, const char *cmd) {
    if (PipeMAIN2CSP[1] == 0) return; // Pipe has not yet been opened (Ooops).
    if (write(PipeMAIN2CSP[1], cmd, strlen(cmd)) != strlen(cmd))
        ppl_error(&context->errcontext, ERR_INTERNAL, -1, -1, "Attempt to send a message to the CSP failed.");
    return;
}

// Functions to be called from the Child Support Process

//! pplcsp_main - The main loop of the CSP, which waits for commands from the main process and for its child
//! processes to exit, until the main process exits, and then deletes the temporary working directory
//! \param [in] context - The Pyxplot context

void pplcsp_main(ppl_context *context) {
    // Keep running while Pyxplot is running, or while any persistent viewers remain open
    int gotPersists = 1;

    // Every 100ms, check for commands from the main process
    while (gotPersists) {
        struct timespec waitperiod, waitedperiod;
        waitperiod.tv_sec = 0;
        waitperiod.tv_nsec = PyxplotRunning
                ? 100000000
                : 500000000; // After pyxplot has quit, reduce polling rate to twice a second. Why not?
        nanosleep(&waitperiod, &waitedperiod); // Wake up every 100ms
        pplcsp_checkForNewCommands(context); // Check for orders from Pyxplot
        if (PyxplotRunning && (getppid() == 1)) // We've been orphaned and adopted by init
        {
            PyxplotRunning = 0;
            if (DEBUG) ppl_log(&context->errcontext, "CSP has detected that it has been orphaned.");
        }
        gotPersists = 0;
        if (PyxplotRunning) gotPersists = 1;
        // After Pyxplot has quit, wait for persistent viewers to close
        else {
            int i;
            for (i = 0; i < PTABLE_LISTLEN; i++)
                // A persistent viewer is still open
                if (GhostView_Persists[i] != 0) {
                    gotPersists = 1;
                    break;
                }
        }
    }
    return;
}

//! pplcsp_checkForChildExits - SIGCHLD handler in the CSP, which records that a child process may have exited
//! \param [in] signo - The signal number

void pplcsp_checkForChildExits(const int signo) {
    // Variables for reaping viewers
    int i;
    char errtext[FNAME_LENGTH];
    ppl_context *context = static_context;

    // Note which viewers have exited
    for (i = 0; i < PTABLE_LISTLEN; i++) {
        const int gv_pid = GhostViews[i];
        if (gv_pid == 0) continue;
        // This viewer has exited
        if (waitpid(gv_pid, NULL, WNOHANG) != 0) {
            // Log the exit in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(errtext, sizeof(errtext), "A ghostview process with pid %d has terminated.", gv_pid);
                ppl_log(&context->errcontext, errtext);
            }
            GhostViews[i] = 0; // Stabat mater dolorosa
            if (GhostView_pid == gv_pid) GhostView_pid = 0;
        }
    }

    // Note which persistent viewers have exited
    for (i = 0; i < PTABLE_LISTLEN; i++) {
        const int gv_pid = GhostView_Persists[i];
        if (gv_pid == 0) continue;
        // This viewer has exited
        if (waitpid(gv_pid, NULL, WNOHANG) != 0) {
            // Log the exit in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(errtext, sizeof(errtext),
                                  "A persistent ghostview process with pid %d has terminated.", gv_pid);
                ppl_log(&context->errcontext, errtext);
            }
            GhostView_Persists[i] = 0; // Stabat mater dolorosa
            if (GhostView_pid == gv_pid) GhostView_pid = 0;
        }
    }
    return;
}

//! pplcsp_checkForNewCommands - Read any commands which the main process has sent to the CSP, and process them
//! \param [in] context - The Pyxplot context

void pplcsp_checkForNewCommands(ppl_context *context) {
    struct timespec waitperiod; // A time.h timespec specifier for a wait of zero seconds
    fd_set readable;
    int pos, TrialNumber;
    ssize_t nread;
    char linebuffer[SSTR_LENGTH];

    // Check whether the pipe from the main process is readable, retrying up to three times if interrupted by a signal
    TrialNumber = 1;
    // Retry if interrupted by a signal
    while (1) {
        waitperiod.tv_sec = waitperiod.tv_nsec = 0;
        FD_ZERO(&readable);
        FD_SET(PipeMAIN2CSP[0], &readable);
        // Report a failure of pselect
        if (pselect(PipeMAIN2CSP[0] + 1, &readable, NULL, NULL, &waitperiod, NULL) == -1) {
            // Retry if interrupted by a signal
            if ((errno == EINTR) && (TrialNumber < 3)) {
                TrialNumber++;
                continue;
            }
            if (DEBUG) ppl_log(&context->errcontext, "pselect failure whilst CSP checking for new commands");
            return;
        }
        break;
    }
    if (!FD_ISSET(PipeMAIN2CSP[0], &readable)) return; // select tells us that pipe from main process is not readable

    // Read what is waiting, and process it command by command
    pos = strlen(PipeOutputBuffer);
    nread = read(PipeMAIN2CSP[0], PipeOutputBuffer + pos, LSTR_LENGTH - pos - 5);
    // read() does not terminate the string, so terminate it after the new bytes
    if (nread > 0) PipeOutputBuffer[pos + nread] = '\0';
    if (nread > 0)
        // Process each complete line
        while (1) {
            ppl_strRemoveCompleteLine(PipeOutputBuffer, linebuffer);
            if (linebuffer[0] == '\0') break;
            pplcsp_processCommand(context, linebuffer);
        }
    return;
}

//! pplcsp_processCommand - Process a command sent to the CSP by the main process: clearing the display, quitting,
//! or displaying a postscript file in a viewer in single-window, multi-window or persistent mode
//! \param [in] context - The Pyxplot context
//! \param [in] in - The command. Its first character identifies the command, and the rest gives its arguments.

void pplcsp_processCommand(ppl_context *context, char *in) {
    // Commands are a single character: A to clear the display, B when Pyxplot quits, and 0, 1 or 2 followed by a viewer
    // command line, for the X11_singlewindow, X11_multiwindow and X11_persist terminals
    char msg[PPL_MSG_LENGTH];
    char cmd[FNAME_LENGTH];

    // Act on the command
    if (in[0] == '\0') return;
    else if (in[0] == 'A') // clear command executed
    {
        if (DEBUG) ppl_log(&context->errcontext, "Received request to clear display.");
        pplcsp_killLatestSinglewindow(context);
    } else if (in[0] == 'B') // Pyxplot quit
    {
        if (DEBUG) ppl_log(&context->errcontext, "Received notice that Pyxplot is quitting.");
        pplcsp_killAllGvs(context);
        PyxplotRunning = 0;
    } else if (in[0] == '0') // gv_singlewindow
    {
        // Pick out filename of eps file to display, which is last command-line argument
        int state = 0, i;
        char *filename = in + 1;
        for (i = 1; in[i] != '\0'; i++)
            // A new word
            if ((((unsigned char) in[i] > ' ') || (in[i] < '\0')) || ((i > 1) && (in[i - 1] == '\\'))) {
                // Remember where the word starts
                if (!state) {
                    filename = in + i;
                    state = 1;
                } // New word
            } else state = 0;

        // If a single-window viewer is open, copy the new file over the one it is watching; otherwise open a new viewer
        if (GhostView_pid > 1) {
            // Log progress in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg),
                                  "Received gv_singlewindow request. Putting into existing window with pid %d.",
                                  GhostView_pid);
                ppl_log(&context->errcontext, msg);
            }
            if (snprintf(cmd, FNAME_LENGTH, "cp -f %s %s", filename, context->pplcsp_ghostView_fname) >= FNAME_LENGTH) {
                if (DEBUG) ppl_log(&context->errcontext, "Filename of postscript document is too long to copy.");
            } else if (system(cmd) != 0)
                // Log the failure in debugging mode
                if (DEBUG) {
                    ppl_log(&context->errcontext,
                            "Failed to copy postscript document into existing gv_singlewindow session.");
                }
        } else {
            if (DEBUG) ppl_log(&context->errcontext, "Received gv_singlewindow request. Making a new window for it.");
            ppl_snprintfTrunc(context->pplcsp_ghostView_fname, sizeof(context->pplcsp_ghostView_fname), "%s", filename);
            GhostView_pid = pplcsp_forkNewGv(context, in + 1, GhostViews);
        }
    } else if (in[0] == '1') // gv_multiwindow
    {
        if (DEBUG) ppl_log(&context->errcontext, "Received gv_multiwindow request.");
        pplcsp_forkNewGv(context, in + 1, GhostViews);
    } else if (in[0] == '2') // gv_persist
    {
        if (DEBUG) ppl_log(&context->errcontext, "Received gv_persist request.");
        pplcsp_forkNewGv(context, in + 1, GhostView_Persists);
    }
    return;
}

//! pplcsp_forkNewGv - Launch a postscript viewer to display a file
//! \param [in] context - The Pyxplot context
//! \param [in] fname - The filename of the postscript file to display
//! \param [in|out] gv_list - The list of viewer processes to add the new process to (may be NULL)
//! \return The process ID of the viewer, or zero on failure

int pplcsp_forkNewGv(ppl_context *context, char *fname, int *gv_list) {
    // Variables for launching the viewer
    char msg[PPL_MSG_LENGTH];
#define MAX_CMDARGS 64

    // The words of the command line
    int pid, i, state = 0, NArgs = 0;
    char *Args[MAX_CMDARGS], ViewerApp[FNAME_LENGTH];
    sigset_t sigs;

    // Split up command line into words
    for (i = 0; fname[i] != '\0'; i++) {
        // Split the command line into words at whitespace which is not escaped
        if ((((unsigned char) fname[i] > ' ') || (fname[i] < '\0')) || ((i > 1) && (fname[i - 1] == '\\'))) {
            if (state) {
            } // In the middle of a word... keep going
            // The start of a new word
            else {
                Args[NArgs++] = fname + i;
                state = 1;
            } // New word
        } else {
            // The end of a word
            if (state) {
                fname[i] = '\0';
                state = 0;
            } // End of word
            else {} // Lots of whitespace
        }

        // There is a fixed limit on the number of words
        if (NArgs == MAX_CMDARGS) {
            FILE *f = fdopen(PipeCSP2MAIN[1], "w");
            if (f == NULL) return 0;
            fprintf(f, "Command for launching postscript viewer contains too many command-line switches.\n");
            fclose(f);
            return 0;
        }
    }
    Args[NArgs] = NULL;

    // The name of the viewer, for error messages
    snprintf(ViewerApp, FNAME_LENGTH, "%s", Args[0]);
    ViewerApp[FNAME_LENGTH - 1] = '\0';

    // Block SIGCHLD while the viewer is launched, so that it is not reaped before it is recorded
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    // Fork the viewer
    if ((pid = fork()) < 0)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not fork a child process for the CSP.");
    // The CSP records the viewer's pid; the child runs the viewer
    else if (pid != 0) {
        // Parent process (i.e. the CSP)
        int i;
        for (i = 0; i < PTABLE_LISTLEN; i++)
            // Record the viewer in the first free slot
            if (gv_list[i] == 0) {
                gv_list[i] = pid;
                break;
            }
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        return pid;
    } else {
        // Child process; about to run postscript viewer
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        ppl_snprintfTrunc(context->errcontext.error_source, sizeof(context->errcontext.error_source), "GV%7d",
                          getpid());
        context->errcontext.session_default.color = SW_ONOFF_OFF;
        // Log progress in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "New postscript viewer process alive; going to view %s.", fname);
            ppl_log(&context->errcontext, msg);
        }
        if (PipeCSP2MAIN[1] != STDERR_FILENO) // Redirect stderr to pipe, so that GhostView doesn't spam terminal
        {
            if (dup2(PipeCSP2MAIN[1], STDERR_FILENO) != STDERR_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stderr to pipe.");
            close(PipeCSP2MAIN[1]);
        }
        if (setpgid(getpid(), getpid()))
            if (DEBUG)
                ppl_log(&context->errcontext,
                        "Failed to set process group ID."); // Make into a process group leader so that we won't catch SIGINT

        // Run postscript viewer
        if (execvp(Args[0], Args) != 0) {
            if (DEBUG) ppl_log(&context->errcontext, "Attempt to execute postscript viewer returned error code.");
        } // Execute postscript viewer
        fprintf(stderr, "Execution of postscript viewer '%s' failed.\n", ViewerApp);
        fflush(stderr); // execvp call should not return
        exit(1);
    }
    return 0;
}

//! pplcsp_killAllGvs - Close all of the postscript viewers which the CSP has launched
//! \param [in] context - The Pyxplot context

void pplcsp_killAllGvs(ppl_context *context) {
    int i;
    if (DEBUG) ppl_log(&context->errcontext, "Killing all ghostview processes.");
    // Close each viewer
    for (i = 0; i < PTABLE_LISTLEN; i++) {
        const int gv_pid = GhostViews[i];
        if (gv_pid == 0) continue;
        kill(gv_pid, SIGTERM); // Dulce et decorum est pro patria mori
        if (GhostView_pid == gv_pid) GhostView_pid = 0;
    }
    return;
}

//! pplcsp_killLatestSinglewindow - Close the postscript viewer most recently launched in single-window mode, so that
//! it can be replaced by a new one
//! \param [in] context - The Pyxplot context

void pplcsp_killLatestSinglewindow(ppl_context *context) {
    char msg[PPL_MSG_LENGTH];
    // Close the viewer, if there is one
    if (GhostView_pid > 1) {
        // Log progress in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Killing latest ghostview singlewindow process with pid %d.",
                              GhostView_pid);
            ppl_log(&context->errcontext, msg);
        }
        kill(GhostView_pid, SIGTERM);
    } else {
        // Log progress in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "No ghostview singlewindow process to kill.");
            ppl_log(&context->errcontext, msg);
        }
    }
    GhostView_pid = 0;
    return;
}

// Facilities for forking helper processes with pipes from the main Pyxplot process

//! pplcsp_checkForHelperExits - SIGCHLD handler in the main process, which reaps helper processes such as LaTeX and
//! input filters which have exited
//! \param [in] signo - The signal number

void pplcsp_checkForHelperExits(const int signo) {
    int i;
    char errtext[256];
    ppl_context *context = static_context;

    // Note which helpers have exited
    for (i = 0; i < PTABLE_LISTLEN; i++) {
        const int pid = HelperPIDs[i];
        if (pid == 0) continue;
        // This helper has exited
        if (waitpid(pid, NULL, WNOHANG) != 0) {
            // Log the exit in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(errtext, sizeof(errtext), "A helper process with pid %d has terminated.", pid);
                ppl_log(&context->errcontext, errtext);
            }
            HelperPIDs[i] = 0; // Stabat mater dolorosa
        }
    }
    return;
}

//! pplcsp_killAllHelpers - Kill all of the helper processes which the main process has launched
//! \param [in] context - The Pyxplot context

void pplcsp_killAllHelpers(const ppl_context *context) {
    int i;
    sigset_t sigs;

    // Block SIGCHLD while helpers are killed
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    // Kill each helper
    for (i = 0; i < PTABLE_LISTLEN; i++) {
        const int pid = HelperPIDs[i];
        if (pid == 0) continue;
        kill(pid, SIGTERM); // Dulce et decorum est pro patria mori
    }
    sigprocmask(SIG_UNBLOCK, &sigs, NULL);
    return;
}

//! pplcsp_forkSed - Launch sed as a helper process, with pipes connected to its standard input and output. SIGCHLD is
//! left blocked; the caller must unblock it.
//! \param [in] context - The Pyxplot context
//! \param [in] cmd - The sed command to run
//! \param [out] fstdin - Set to the file descriptor of a pipe to sed's standard input
//! \param [out] fstdout - Set to the file descriptor of a pipe from sed's standard output

void pplcsp_forkSed(ppl_context *context, char *cmd, int *fstdin, int *fstdout) {
    // Variables for launching the helper
    char msg[PPL_MSG_LENGTH];
    int fd0[2], fd1[2];
    int i, pid;
    sigset_t sigs;

    // SIGCHLD is blocked while the helper is launched, so that it is not reaped before it is recorded
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);

    // Create the pipes to the helper's standard input and output
    if ((pipe(fd0) < 0) || (pipe(fd1) < 0))
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not open required pipes.");

    // Block SIGCHLD
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    // Fork the helper
    if ((pid = fork()) < 0)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not fork a child process for sed process.");
    // The main process keeps its ends of the pipes and records the helper's pid; the child redirects its input and
    // output to the pipes and runs sed
    else if (pid != 0) {
        // Parent process
        close(fd0[0]);
        *fstdin = fd0[1];
        close(fd1[1]);
        *fstdout = fd1[0];
        for (i = 0; i < PTABLE_LISTLEN; i++)
            // Record the helper in the first free slot
            if (HelperPIDs[i] == 0) {
                HelperPIDs[i] = pid;
                break;
            }
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        return;
    } else {
        // Child process
        close(fd0[1]);
        close(fd1[0]);
        close(PipeCSP2MAIN[0]);
        close(PipeMAIN2CSP[1]);
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        ppl_snprintfTrunc(context->errcontext.error_source, sizeof(context->errcontext.error_source), "SED%6d",
                          getpid());
        context->errcontext.session_default.color = SW_ONOFF_OFF;
        // Log progress in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "New sed process alive; going to run command \"%s\".", cmd);
            ppl_log(&context->errcontext, msg);
        }
        if (fd0[0] != STDIN_FILENO) // Redirect stdin to pipe
        {
            if (dup2(fd0[0], STDIN_FILENO) != STDIN_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stdin to pipe.");
            close(fd0[0]);
        }
        if (fd1[1] != STDOUT_FILENO) // Redirect stdout to pipe
        {
            if (dup2(fd1[1], STDOUT_FILENO) != STDOUT_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stdout to pipe.");
            close(fd1[1]);
        }
        if (PipeCSP2MAIN[1] != STDERR_FILENO) // Redirect stderr to pipe
        {
            if (dup2(PipeCSP2MAIN[1], STDERR_FILENO) != STDERR_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stderr to pipe.");
            close(PipeCSP2MAIN[1]);
        }
        if (execl(SED_COMMAND, SED_COMMAND, cmd, NULL) != 0)
            if (DEBUG) ppl_log(&context->errcontext, "Attempt to execute sed returned error code."); // Execute sed
        ppl_error(&context->errcontext, ERR_GENERIC, -1, -1,
                  "Execution of helper process 'sed' failed."); // execlp call should not return
        exit(1);
    }
    return;
}

//! pplcsp_forkLaTeX - Launch LaTeX as a helper process to typeset a file, with pipes connected to its standard input
//! and output. SIGCHLD is left blocked; the caller must unblock it.
//! \param [in] context - The Pyxplot context
//! \param [in] filename - The filename of the LaTeX document to typeset
//! \param [out] PidOut - Set to the process ID of the LaTeX process
//! \param [out] fstdin - Set to the file descriptor of a pipe to LaTeX's standard input
//! \param [out] fstdout - Set to the file descriptor of a pipe from LaTeX's standard output

void pplcsp_forkLaTeX(ppl_context *context, char *filename, int *PidOut, int *fstdin, int *fstdout) {
    // Variables for launching the helper
    char msg[PPL_MSG_LENGTH];
    int fd0[2], fd1[2];
    int i, pid;
    sigset_t sigs;

    // SIGCHLD is blocked while the helper is launched, so that it is not reaped before it is recorded
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);

    // Create the pipes to the helper's standard input and output
    if ((pipe(fd0) < 0) || (pipe(fd1) < 0))
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not open required pipes.");

    // Block SIGCHLD
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    // Fork the helper
    if ((pid = fork()) < 0)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not fork a child process for latex process.");
    // The main process keeps its ends of the pipes and records the helper's pid; the child redirects its input and
    // output to the pipes and runs latex
    else if (pid != 0) {
        // Parent process
        close(fd0[0]);
        *fstdin = fd0[1];
        close(fd1[1]);
        *fstdout = fd1[0];
        *PidOut = pid;
        for (i = 0; i < PTABLE_LISTLEN; i++)
            // Record the helper in the first free slot
            if (HelperPIDs[i] == 0) {
                HelperPIDs[i] = pid;
                break;
            }
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        return;
    } else {
        // Child process
        close(fd0[1]);
        close(fd1[0]);
        close(PipeCSP2MAIN[0]);
        close(PipeMAIN2CSP[1]);
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        ppl_snprintfTrunc(context->errcontext.error_source, sizeof(context->errcontext.error_source), "TEX%6d",
                          getpid());
        context->errcontext.session_default.color = SW_ONOFF_OFF;
        // Log progress in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "New latex process alive; going to latex file \"%s\".", filename);
            ppl_log(&context->errcontext, msg);
        }
        if (fd0[0] != STDIN_FILENO) // Redirect stdin to pipe
        {
            if (dup2(fd0[0], STDIN_FILENO) != STDIN_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stdin to pipe.");
            close(fd0[0]);
        }
        if (fd1[1] != STDOUT_FILENO) // Redirect stdout to pipe
        {
            if (dup2(fd1[1], STDOUT_FILENO) != STDOUT_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stdout to pipe.");
        }
        if (PipeCSP2MAIN[1] != STDERR_FILENO) // Redirect stderr to pipe
        {
            if (dup2(fd1[1], STDERR_FILENO) != STDERR_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stderr to pipe.");
            close(fd1[1]);
        }
        if (execl(LATEX_COMMAND, LATEX_COMMAND, "-file-line-error", filename, NULL) != 0)
            if (DEBUG) ppl_log(&context->errcontext, "Attempt to execute latex returned error code."); // Execute latex
        ppl_error(&context->errcontext, ERR_GENERIC, -1, -1,
                  "Execution of helper process 'latex' failed."); // execlp call should not return
        exit(1);
    }
    return;
}

//! pplcsp_forkInputFilter - Launch an input filter as a helper process, to preprocess a data file. SIGCHLD is left
//! blocked; the caller must unblock it.
//! \param [in] context - The Pyxplot context
//! \param [in] cmd - NULL-terminated argument list of the filter command, the last argument being the data file
//! \param [out] fstdout - Set to the file descriptor of a pipe from the filter's standard output

void pplcsp_forkInputFilter(ppl_context *context, char **cmd, int *fstdout) {
    // Variables for launching the helper
    char msg[PPL_MSG_LENGTH];
    int fd0[2];
    int i, pid;
    sigset_t sigs;

    // SIGCHLD is blocked while the helper is launched, so that it is not reaped before it is recorded
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);

    // Create the pipe from the helper's standard output
    if (pipe(fd0) < 0) ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not open required pipes.");

    // Block SIGCHLD
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    // Fork the helper
    if ((pid = fork()) < 0)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not fork a child process for input filter process.");
    // The main process keeps its ends of the pipes and records the helper's pid; the child redirects its input and
    // output to the pipes and runs the input filter
    else if (pid != 0) {
        // Parent process
        close(fd0[1]);
        *fstdout = fd0[0];
        for (i = 0; i < PTABLE_LISTLEN; i++)
            // Record the helper in the first free slot
            if (HelperPIDs[i] == 0) {
                HelperPIDs[i] = pid;
                break;
            }
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        return;
    } else {
        // Child process
        close(fd0[0]);
        close(PipeCSP2MAIN[0]);
        close(PipeMAIN2CSP[1]);
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        ppl_snprintfTrunc(context->errcontext.error_source, sizeof(context->errcontext.error_source), "IF %6d",
                          getpid());
        context->errcontext.session_default.color = SW_ONOFF_OFF;
        // Log progress in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "New input filter process alive; going to run command \"%s\".", cmd[0]);
            ppl_log(&context->errcontext, msg);
        }
        if (fd0[1] != STDOUT_FILENO) // Redirect stdout to pipe
        {
            if (dup2(fd0[1], STDOUT_FILENO) != STDOUT_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stdout to pipe.");
            close(fd0[1]);
        }
        if (PipeCSP2MAIN[1] != STDERR_FILENO) // Redirect stderr to pipe
        {
            if (dup2(PipeCSP2MAIN[1], STDERR_FILENO) != STDERR_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stderr to pipe.");
            close(PipeCSP2MAIN[1]);
        }
        if (execvp(cmd[0], cmd) != 0)
            if (DEBUG)
                ppl_log(&context->errcontext,
                        "Attempt to execute input filter returned error code."); // Execute input filter
        ppl_snprintfTrunc(msg, sizeof(msg), "Execution of input filter '%s' failed.", cmd[0]);
        ppl_error(&context->errcontext, ERR_GENERIC, -1, -1, msg); // execvp call should not return
        exit(1);
    }
    return;
}

//! pplcsp_forkKpseWhich - Launch kpsewhich as a helper process, to list the search path for a type of TeX file.
//! SIGCHLD is left blocked; the caller must unblock it.
//! \param [in] context - The Pyxplot context
//! \param [in] ftype - The type of file to search for, e.g. "tfm" or "pfa"
//! \param [out] fstdout - Set to the file descriptor of a pipe from kpsewhich's standard output

void pplcsp_forkKpseWhich(ppl_context *context, const char *ftype, int *fstdout) {
    // Variables for launching the helper
    char msg[PPL_MSG_LENGTH];
    char CmdLineOpt[128];
    int fd0[2];
    int i, pid;
    sigset_t sigs;

    // SIGCHLD is blocked while the helper is launched, so that it is not reaped before it is recorded
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);

    // Create the pipe from the helper's standard output
    if (pipe(fd0) < 0) ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not open required pipes.");

    // Block SIGCHLD
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    // Fork the helper
    if ((pid = fork()) < 0)
        ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not fork a child process for kpsewhich process.");
    // The main process keeps its ends of the pipes and records the helper's pid; the child redirects its input and
    // output to the pipes and runs kpsewhich
    else if (pid != 0) {
        // Parent process
        close(fd0[1]);
        *fstdout = fd0[0];
        for (i = 0; i < PTABLE_LISTLEN; i++)
            // Record the helper in the first free slot
            if (HelperPIDs[i] == 0) {
                HelperPIDs[i] = pid;
                break;
            }
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        return;
    } else {
        // Child process
        close(fd0[0]);
        close(PipeCSP2MAIN[0]);
        close(PipeMAIN2CSP[1]);
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);
        ppl_snprintfTrunc(context->errcontext.error_source, sizeof(context->errcontext.error_source), "KPS%6d",
                          getpid());
        context->errcontext.session_default.color = SW_ONOFF_OFF;
        // Log progress in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "New kpsewhich process alive; going to get paths for filetype \"%s\".",
                              ftype);
            ppl_log(&context->errcontext, msg);
        }
        if (fd0[1] != STDOUT_FILENO) // Redirect stdout to pipe
        {
            if (dup2(fd0[1], STDOUT_FILENO) != STDOUT_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stdout to pipe.");
            close(fd0[1]);
        }
        if (PipeCSP2MAIN[1] != STDERR_FILENO) // Redirect stderr to pipe
        {
            if (dup2(PipeCSP2MAIN[1], STDERR_FILENO) != STDERR_FILENO)
                ppl_fatal(&context->errcontext, __FILE__, __LINE__, "Could not redirect stderr to pipe.");
            close(PipeCSP2MAIN[1]);
        }
        ppl_snprintfTrunc(CmdLineOpt, sizeof(CmdLineOpt), "-show-path=.%s", ftype);
        if (execl(KPSE_COMMAND, KPSE_COMMAND, CmdLineOpt, NULL) != 0)
            if (DEBUG)
                ppl_log(&context->errcontext, "Attempt to execute kpsewhich returned error code."); // Execute kpsewhich
        ppl_error(&context->errcontext, ERR_GENERIC, -1, -1,
                  "Execution of helper process 'kpsewhich' failed."); // execlp call should not return
        exit(1);
    }
    return;
}
