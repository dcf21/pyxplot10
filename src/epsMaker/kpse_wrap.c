// kpse_wrap.c
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

// Finds the TeX font files (tfm, pfa, pfb, map and enc) needed to convert LaTeX's dvi output into postscript, using the
// kpathsea library if it is available. Otherwise, at startup, it runs kpsewhich once for each type of file to learn the
// directories in which TeX searches for it, and then searches those directories itself, recursing into the
// subdirectories of those marked with !!.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "epsMaker/kpse_wrap.h"
#include "stringTools/strConstants.h"
#include "userspace/context.h"

#ifdef HAVE_KPATHSEA
#include <kpathsea/kpathsea.h>
#else
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include "children.h"
#define MAX_PATHS    256
#define N_FILE_TYPES 5 // TFM, PFA, PFB, font map and encoding files
char ppl_kpse_FilePaths[N_FILE_TYPES][LSTR_LENGTH];
char *ppl_kpse_PathList[N_FILE_TYPES][MAX_PATHS];
unsigned char ppl_kpse_PathRecursive[N_FILE_TYPES][MAX_PATHS];
#endif

//! ppl_kpse_wrap_init - Read the search paths for TeX font files (TFM, PFA, PFB, font maps and encodings) by running
//! kpsewhich, for use when
//! looking up fonts
//! \param [in] c - The Pyxplot context

void ppl_kpse_wrap_init(ppl_context *c) {
#ifdef HAVE_KPATHSEA
    kpse_set_program_name("dvips", "dvips");
#else
    char msg[PPL_MSG_LENGTH];
    const char *FileTypes[N_FILE_TYPES] = {"tfm", "pfa", "pfb", "map", "enc"};
    int i, k, s, fstdout;
    struct timespec waitperiod; // A time.h timespec specifier for a wait of zero seconds
    fd_set readable;
    sigset_t sigs;
    pplerr_context *ec = &c->errcontext;

    // SIGCHLD is unblocked again after each kpsewhich process has been read
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);

    // Start with empty lists of paths
    for (int j = 0; j < N_FILE_TYPES; j++) {
        ppl_kpse_PathList[j][0] = NULL;
        for (i = 0; i < MAX_PATHS; i++) ppl_kpse_PathRecursive[j][i] = 0;
    }

    // Ask kpsewhich for the search path of each type of file
    for (int j = 0; j < N_FILE_TYPES; j++) {
        pplcsp_forkKpseWhich(c, FileTypes[j], &fstdout);

        // Wait for kpsewhich process's stdout to become readable. Get bored if this takes more than a second.
        int TrialNumber = 1;
        // Retry the wait up to three times if it is interrupted by a signal
        while (1) {
            waitperiod.tv_sec = 1;
            waitperiod.tv_nsec = 0;
            FD_ZERO(&readable);
            FD_SET(fstdout, &readable);
            // Wait for output, or a timeout
            if (pselect(fstdout + 1, &readable, NULL, NULL, &waitperiod, NULL) == -1) {
                // Retry if interrupted by a signal
                if ((errno == EINTR) && (TrialNumber < 3)) {
                    TrialNumber++;
                    continue;
                }
                ppl_error(ec, ERR_INTERNAL, -1, -1,
                          "Failure of the pselect() function whilst waiting for kpsewhich to return data.");
                return;
            }
            break;
        }
        // Give up on this type of file if kpsewhich did not respond
        if (!FD_ISSET(fstdout, &readable)) {
            ppl_error(ec, ERR_GENERIC, -1, -1, "Got bored waiting for kpsewhich to return data.");
            sigprocmask(SIG_UNBLOCK, &sigs, NULL);
            continue;
        }

        // Read data back from kpsewhich process
        if ((i = read(fstdout, ppl_kpse_FilePaths[j], LSTR_LENGTH)) < 0) {
            ppl_error(ec, ERR_GENERIC, -1, -1, "Could not read from pipe to kpsewhich.");
            sigprocmask(SIG_UNBLOCK, &sigs, NULL);
            continue;
        }
        ppl_kpse_FilePaths[j][i] = '\0';
        close(fstdout);
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);

        // Split up returned data into a list of paths
        for (i = s = k = 0; ppl_kpse_FilePaths[j][i] != '\0'; i++) {
            // A path starting with !! is searched recursively
            if (ppl_kpse_FilePaths[j][i] == '!') {
                if (!s) ppl_kpse_PathRecursive[j][k] = 1;
                continue;
            }
            // A colon or newline ends a path; strip any trailing slashes, and terminate it
            if ((ppl_kpse_FilePaths[j][i] == ':') || (ppl_kpse_FilePaths[j][i] == '\n')) {
                for (int l = i - 1; ((l >= 0) && (ppl_kpse_FilePaths[j][l] == PATHLINK[0])); l--)
                    ppl_kpse_FilePaths[j][l] = '\0';
                s = 0;
                ppl_kpse_FilePaths[j][i] = '\0';
                continue;
            }
            // The first character of a new path
            if (!s) {
                s = 1;
                ppl_kpse_PathList[j][k++] = ppl_kpse_FilePaths[j] + i;
                // There is a fixed limit on the number of paths
                if (k == MAX_PATHS) {
                    k--;
                    ppl_error(ec, ERR_GENERIC, -1, -1, "kpsewhich returned too many paths");
                }
            }
        }
        ppl_kpse_PathList[j][k] = NULL;

        // If debugging, log a list of the paths that we've extracted
        if (DEBUG) {
            // Log each path
            for (i = 0; ppl_kpse_PathList[j][i] != NULL; i++) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Using path for %s files: <%s> [%srecursive]", FileTypes[j],
                                  ppl_kpse_PathList[j][i], ppl_kpse_PathRecursive[j][i] ? "" : "non-");
                ppl_log(ec, msg);
            }
        }
    }
#endif
    return;
}

#ifndef HAVE_KPATHSEA

//! ppl_kpse_wrap_test_path - Test whether a file exists in a directory, optionally searching subdirectories recursively
//! \param [in] ec - The error-reporting context
//! \param [in] s - The filename to search for
//! \param [in] path - The directory to search in
//! \param [in] recurse - If true, also search subdirectories
//! \return The full path of the file, stored in a static buffer, or NULL if it was not found

char *ppl_kpse_wrap_test_path(pplerr_context *ec, char *s, char *path, unsigned char recurse) {
    // The result is returned in a static buffer, shared by the recursive calls
    char msg[PPL_MSG_LENGTH];
    DIR *dp;
    struct dirent *dirp;
    struct stat statbuf;
    static char buffer[FNAME_LENGTH], next[FNAME_LENGTH];
    static unsigned char found;

    if (path[0] == '.') return NULL; // Do not look for files in cwd
    found = 0;
    snprintf(buffer, FNAME_LENGTH, "%s%s%s", path, PATHLINK, s);
    buffer[FNAME_LENGTH - 1] = '\0';
    // The file is in this directory
    if (access(buffer, R_OK) == 0) {
        // Log the file found
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "KPSE found file <%s>", buffer);
            ppl_log(ec, msg);
        }
        found = 1;
        return buffer;
    }
    // Otherwise, search each subdirectory, until the file is found
    if (recurse) {
        snprintf(buffer, FNAME_LENGTH, "%s%s", path, PATHLINK);
        buffer[FNAME_LENGTH - 1] = '\0';
        int pos = strlen(buffer);
        if ((dp = opendir(buffer)) == NULL) return NULL;
        // Search each subdirectory
        while (((dirp = readdir(dp)) != NULL) && (!found)) {
            if ((strcmp(dirp->d_name, ".") == 0) || (strcmp(dirp->d_name, "..") == 0)) continue;
            buffer[pos] = '\0';
            snprintf(next, FNAME_LENGTH, "%s%s", buffer, dirp->d_name);
            next[FNAME_LENGTH - 1] = '\0';
            if (lstat(next, &statbuf) < 0) continue;
            if (S_ISDIR(statbuf.st_mode) == 0) continue;
            ppl_kpse_wrap_test_path(ec, s, next, 1);
        }
        closedir(dp);
    }
    return found ? buffer : NULL;
}

//! ppl_kpse_wrap_find_file - Search a list of directories for a file
//! \param [in] ec - The error-reporting context
//! \param [in] s - The filename to search for
//! \param [in] paths - NULL-terminated array of the directories to search
//! \param [in] RecurseList - Array of flags indicating whether each directory should be searched recursively
//! \return The full path of the file, stored in a static buffer, or NULL if it was not found

char *ppl_kpse_wrap_find_file(pplerr_context *ec, char *s, char **paths, unsigned char *RecurseList) {
    char msg[PPL_MSG_LENGTH];
    // Log the search
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Searching for file <%s>", s);
        ppl_log(ec, msg);
    }
    // Search each directory in turn
    for (int i = 0; paths[i] != NULL; i++) {
        char *output = ppl_kpse_wrap_test_path(ec, s, paths[i], RecurseList[i]);
        if (output != NULL) return output;
    }
    return NULL;
}
#endif

//! ppl_kpse_wrap_find_pfa - Search the TeX font search path for a PFA font file
//! \param [in] ec - The error-reporting context
//! \param [in] s - The filename to search for
//! \return The full path of the file, stored in a static buffer, or NULL if it was not found

char *ppl_kpse_wrap_find_pfa(pplerr_context *ec, char *s) {
#ifdef HAVE_KPATHSEA
    return (char *) kpse_find_file(s, kpse_type1_format, true);
#else
    return ppl_kpse_wrap_find_file(ec, s, ppl_kpse_PathList[1], ppl_kpse_PathRecursive[1]);
#endif
}

//! ppl_kpse_wrap_find_pfb - Search the TeX font search path for a PFB font file
//! \param [in] ec - The error-reporting context
//! \param [in] s - The filename to search for
//! \return The full path of the file, stored in a static buffer, or NULL if it was not found

char *ppl_kpse_wrap_find_pfb(pplerr_context *ec, char *s) {
#ifdef HAVE_KPATHSEA
    return (char *) kpse_find_file(s, kpse_type1_format, true);
#else
    return ppl_kpse_wrap_find_file(ec, s, ppl_kpse_PathList[2], ppl_kpse_PathRecursive[2]);
#endif
}

//! ppl_kpse_wrap_find_tfm - Search the TeX font search path for a TFM font metric file
//! \param [in] ec - The error-reporting context
//! \param [in] s - The filename to search for
//! \return The full path of the file, stored in a static buffer, or NULL if it was not found

char *ppl_kpse_wrap_find_tfm(pplerr_context *ec, char *s) {
#ifdef HAVE_KPATHSEA
    return (char *) kpse_find_tfm(s);
#else
    return ppl_kpse_wrap_find_file(ec, s, ppl_kpse_PathList[0], ppl_kpse_PathRecursive[0]);
#endif
}

//! ppl_kpse_wrap_find_map - Search the TeX font search path for a font map file, such as psfonts.map
//! \param [in] ec - The error-reporting context
//! \param [in] s - The filename to search for
//! \return The full path of the file, stored in a static buffer, or NULL if it was not found

char *ppl_kpse_wrap_find_map(pplerr_context *ec, char *s) {
#ifdef HAVE_KPATHSEA
    return (char *) kpse_find_file(s, kpse_fontmap_format, true);
#else
    return ppl_kpse_wrap_find_file(ec, s, ppl_kpse_PathList[3], ppl_kpse_PathRecursive[3]);
#endif
}

//! ppl_kpse_wrap_find_enc - Search the TeX font search path for a postscript font encoding (.enc) file
//! \param [in] ec - The error-reporting context
//! \param [in] s - The filename to search for
//! \return The full path of the file, stored in a static buffer, or NULL if it was not found

char *ppl_kpse_wrap_find_enc(pplerr_context *ec, char *s) {
#ifdef HAVE_KPATHSEA
    return (char *) kpse_find_file(s, kpse_enc_format, true);
#else
    return ppl_kpse_wrap_find_file(ec, s, ppl_kpse_PathList[4], ppl_kpse_PathRecursive[4]);
#endif
}
