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

// Functions for getting information out of /etc/passwd

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
    int uid;
    struct passwd *ptr;

    uid = getuid();
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
    struct passwd *ptr;
    static char *failover = NULL;
    ptr = UnixGetPwEntry(context);
    if (ptr == NULL) {
        if (failover != NULL) return failover;
        failover = malloc(FNAME_LENGTH);
        if (failover == NULL)
            ppl_fatal(context, __FILE__, __LINE__, "Could not find user's entry in output of getpwent()");
        sprintf(failover, "/home/%s", getlogin());
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
//! \param [in] context - The error-reporting context; its <tempErrStr> buffer is used to hold the result
//! \return The user's real name, or their login name if it could not be found. Overwritten by subsequent calls.

char *ppl_unixGetIRLName(pplerr_context *context) {
    struct passwd *ptr;
    int i;
    ptr = UnixGetPwEntry(context);
    if (ptr == NULL) return getlogin(); // In case of failure, return user's login.
    strcpy(context->tempErrStr, ptr->pw_gecos);
    for (i = 0; context->tempErrStr[i] != '\0'; i++)
        if (context->tempErrStr[i] == ',') context->tempErrStr[i] = '\0'; // Remove commas from in-real-life name
    return context->tempErrStr;
}

//! ppl_unixExpandUserHomeDir - Expand a filename into an absolute path, expanding ~ and ~user into the relevant home
//! directories, and prefixing relative paths with the current working directory
//! \param [in] context - The error-reporting context
//! \param [in] in - The filename to expand, e.g. ~dcf21/script.ppl
//! \param [in] cwd - The current working directory
//! \param [out] out - Character buffer into which to write the expanded path (at least FNAME_LENGTH bytes)
//! \return Pointer to <out>

char *ppl_unixExpandUserHomeDir(pplerr_context *context, char *in, char *cwd, char *out) {
    char UserName[1024];
    char *scan_in, *scan_out;
    if (in[0] == '/') strcpy(out, in);
    else if (in[0] != '~') sprintf(out, "%s%s%s", cwd, PATHLINK, in);
    else if (in[1] == '/') sprintf(out, "%s%s%s", ppl_unixGetHomeDir(context), PATHLINK, in + 2);
    else {
        for (scan_in = in + 1, scan_out = UserName; ((*scan_in != '/') && (*scan_in != '\0'));
             *(scan_out++) = *(scan_in++));
        *scan_out = '\0';
        scan_out = ppl_unixGetUserHomeDir(context, UserName);
        if (scan_out != NULL) sprintf(out, "%s%s", scan_out, scan_in);
        else strcpy(out, in);
    }
    return out;
}
