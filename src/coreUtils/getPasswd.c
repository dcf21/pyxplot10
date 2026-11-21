// ppl_passwd.c
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

// Looks up information about users in the system's password database: the current user's home directory and real name,
// and the home directories of other users. It is used to expand filenames such as ~/data.dat, and to find the user's
// configuration file.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "pplConstants.h"

//! UnixGetPwEntry - Look up the current user's entry in the password database
//! \param [in] context - The error-reporting context (unused)
//! \return Pointer to the current user's passwd structure, which is a static buffer owned by the C library, or NULL
//! if the current user has no entry

struct passwd *UnixGetPwEntry(const pplerr_context *context) {
    struct passwd *ptr;

    // Search the password database for an entry with the current user's ID
    int uid = getuid();
    setpwent(); // Memory leak which valgrind reveals here is probably okay; getpwent sets up a static variable with malloc
    while ((ptr = getpwent()) != NULL)
        if (ptr->pw_uid == uid) break;
    endpwent();
    return (ptr);
}

//! ppl_unixGetHomeDir - Return the path of the current user's home directory. If the user has no entry in the
//! password database, a warning is issued and /home/<login name> is guessed.
//! \param [in] context - The error-reporting context, used to issue warnings and fatal errors
//! \return The path of the current user's home directory, stored in a buffer which must not be freed

char *ppl_unixGetHomeDir(pplerr_context *context) {
    char msg[PPL_MSG_LENGTH];
    static char *failover = NULL;
    struct passwd *ptr = UnixGetPwEntry(context);
    // If the user has no entry, guess that their home directory is /home/<login name>, and warn about it once
    if (ptr == NULL) {
        if (failover != NULL) return failover;
        failover = malloc(FNAME_LENGTH);
        if (failover == NULL)
            ppl_fatal(context, __FILE__, __LINE__, "Could not find user's entry in output of getpwent()");
        ppl_snprintfTrunc(failover, FNAME_LENGTH, "/home/%s", getlogin());
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Could not find user's entry in output of getpwent(). Guessing that home directory is <%s>.",
                          failover);
        ppl_warning(context, ERR_GENERIC, msg);
        return failover;
    }
    return ptr->pw_dir;
}

//! ppl_unixGetUserHomeDir - Return the path of a named user's home directory
//! \param [in] context - The error-reporting context (unused)
//! \param [in] username - The login name of the user whose home directory we are to look up
//! \return The path of the user's home directory, stored in a static buffer owned by the C library, or NULL if no
//! such user exists

char *ppl_unixGetUserHomeDir(const pplerr_context *context, const char *username) {
    struct passwd *ptr;
    setpwent(); // Ditto memory leak comment above
    while ((ptr = getpwent()) != NULL)
        if (strcmp(ptr->pw_name, username) == 0) break;
    endpwent();
    if (ptr == NULL) return NULL;
    return ptr->pw_dir;
}

//! ppl_unixGetIRLName - Return the current user's real name, taken from the GECOS field of the password database,
//! with any text after the first comma removed
//! \param [in] context - The error-reporting context
//! \return The user's real name, stored in a static buffer which is overwritten by subsequent calls, or their login
//! name if it could not be found

char *ppl_unixGetIRLName(pplerr_context *context) {
    static char name[FNAME_LENGTH];
    int i;
    struct passwd *ptr = UnixGetPwEntry(context);
    if (ptr == NULL) return getlogin(); // In case of failure, return user's login.
    snprintf(name, FNAME_LENGTH, "%s", ptr->pw_gecos);
    for (i = 0; name[i] != '\0'; i++)
        if (name[i] == ',') name[i] = '\0'; // Remove commas from in-real-life name
    return name;
}

//! ppl_unixExpandUserHomeDir - Expand a filename into an absolute path, expanding ~ and ~user into the relevant home
//! directories, and prefixing relative paths with the current working directory
//! \param [in] context - The error-reporting context
//! \param [in] in - The filename to expand, e.g. ~dcf21/script.ppl
//! \param [in] cwd - The current working directory
//! \param [out] out - Character buffer into which to write the expanded path, which is truncated if necessary
//! \param [in] outLen - The size of the buffer <out>
//! \return Pointer to <out>

char *ppl_unixExpandUserHomeDir(pplerr_context *context, char *in, char *cwd, char *out, const size_t outLen) {
    char userName[1024];
    char *scanIn, *homeDir;
    int i;
    if (in[0] == '/') ppl_snprintfTrunc(out, outLen, "%s", in);
    else if (in[0] != '~') ppl_snprintfTrunc(out, outLen, "%s%s%s", cwd, PATHLINK, in);
    else if (in[1] == '/') ppl_snprintfTrunc(out, outLen, "%s%s%s", ppl_unixGetHomeDir(context), PATHLINK, in + 2);
    // ~user/path: look up the home directory of the named user
    else {
        // ~user/path: look up the home directory of the named user. Names too long to be valid are left unexpanded.
        for (scanIn = in + 1, i = 0; ((*scanIn != '/') && (*scanIn != '\0') && (i < (int) sizeof(userName) - 1));
             userName[i++] = *(scanIn++));
        userName[i] = '\0';
        homeDir = ((*scanIn == '/') || (*scanIn == '\0')) ? ppl_unixGetUserHomeDir(context, userName) : NULL;
        if (homeDir != NULL) ppl_snprintfTrunc(out, outLen, "%s%s", homeDir, scanIn);
        else ppl_snprintfTrunc(out, outLen, "%s", in);
    }
    return out;
}
