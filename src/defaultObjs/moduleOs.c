// moduleOs.c
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

// The os module, which gives scripts access to the operating system: the working directory, environment, user and host
// names, process IDs, running shell commands and pipes, listing files with wildcards, and the os.path functions which
// inspect filenames and files. These functions have side effects, so are never called from several threads at once.

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <wordexp.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include "coreUtils/dict.h"
#include "coreUtils/getPasswd.h"
#include "settings/settings.h"
#include "stringTools/asciidouble.h"

#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"

#include "defaultObjs/moduleOs.h"
#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsChecks.h"


//! pplfunc_osChdir - Implement the function os.chdir(x) in the os module, which changes working directory to x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osChdir(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The argument must be a single string, the directory
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The os.chdir() function requires a single string argument.");
        return;
    }
    // Report a failure to change directory
    if (chdir((char *) in[0].auxil) != 0) {
        ppl_funcError(status, errType, errText, ERR_FILE, "The os.chdir() encountered an error: %s", strerror(errno));
        return;
    }
    // Record the absolute path of the new working directory, to which Pyxplot returns after, e.g., making plots in a
    // temporary directory. The path supplied may be relative.
    if (getcwd(c->errcontext.session_default.cwd, FNAME_LENGTH) == NULL) {
        strncpy(c->errcontext.session_default.cwd, (char *) in[0].auxil, FNAME_LENGTH);
        c->errcontext.session_default.cwd[FNAME_LENGTH - 1] = '\0';
    }
}

//! pplfunc_osGetCwd - Implement the function os.getcwd() in the os module, which returns the path of the current
//! working directory
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetCwd(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplFuncCall call = {c, in, status, errType, errText, "pplfunc_osGetCwd()"};
    char *tmp;
    if ((tmp = ppl_funcStrdup(&call, c->errcontext.session_default.cwd)) == NULL) return;
    pplObjStr(&OUTPUT, 0, 1, tmp);
}

//! pplfunc_osGetEgid - Implement the function os.getegid() in the os module, which returns the effective group id of
//! the Pyxplot process
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetEgid(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjNum(&OUTPUT, 0, getegid(), 0);
}

//! pplfunc_osGetEuid - Implement the function os.geteuid() in the os module, which returns the effective user id of the
//! Pyxplot process
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetEuid(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjNum(&OUTPUT, 0, geteuid(), 0);
}

//! pplfunc_osGetGid - Implement the function os.getgid() in the os module, which returns the group id of the Pyxplot
//! process
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetGid(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjNum(&OUTPUT, 0, getgid(), 0);
}

//! pplfunc_osGetPid - Implement the function os.getpid() in the os module, which returns the process id of the Pyxplot
//! process
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetPid(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjNum(&OUTPUT, 0, getpid(), 0);
}

//! pplfunc_osGetPgrp - Implement the function os.getpgrp() in the os module, which returns the process group id of the
//! Pyxplot process
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetPgrp(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjNum(&OUTPUT, 0, getpgrp(), 0);
}

//! pplfunc_osGetPpid - Implement the function os.getppid() in the os module, which returns the parent process id of the
//! Pyxplot process
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetPpid(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjNum(&OUTPUT, 0, getppid(), 0);
}

//! pplfunc_osGetUid - Implement the function os.getuid() in the os module, which returns the user id of the Pyxplot
//! process
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetUid(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjNum(&OUTPUT, 0, getuid(), 0);
}

//! pplfunc_osGetHome - Implement the function os.gethomedir() in the os module, which returns the path of the user's
//! home directory
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetHome(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *tmp = ppl_unixGetHomeDir(&c->errcontext);
    char *out = (char *) malloc(strlen(tmp) + 1);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    strcpy(out, tmp);
    pplObjStr(&OUTPUT, 0, 1, out);
    return;
}

//! pplfunc_osGetHost - Implement the function os.gethostname() in the os module, which returns the system's host name
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetHost(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const int outlen = FNAME_LENGTH;
    char *out = (char *) malloc(outlen);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    gethostname(out, outlen);
    out[outlen - 1] = '\0';
    pplObjStr(&OUTPUT, 0, 1, out);
    return;
}

//! pplfunc_osGetLogin - Implement the function os.getlogin() in the os module, which returns the system login of the
//! user
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetLogin(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *tmp = getlogin();
    char *out = (char *) malloc(strlen(tmp) + 1);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    strcpy(out, tmp);
    pplObjStr(&OUTPUT, 0, 1, out);
    return;
}

//! pplfunc_osGetRealName - Implement the function os.getrealname() in the os module, which returns the user's real name
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGetRealName(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *tmp = ppl_unixGetIRLName(&c->errcontext);
    char *out = (char *) malloc(strlen(tmp) + 1);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    strcpy(out, tmp);
    pplObjStr(&OUTPUT, 0, 1, out);
    return;
}

//! pplfunc_osGlob - Implement the function os.glob(x) in the os module, which returns a list of files which match the
//! supplied wildcard
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osGlob(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The pattern is first expanded by wordexp(), which handles ~ and environment variables, and then each word by
    // glob(), which handles wildcards
    pplFuncCall call = {c, in, status, errType, errText, "pplfunc_osGlob()"};
    wordexp_t w;
    glob_t g;
    pplObj v;
    char *tmp;
    char escaped[FNAME_LENGTH], *raw = (char *) in[0].auxil;
    // The argument must be a single string, the pattern
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The os.glob() function requires a single string argument.");
        return;
    }
    // The output is a list of matching filenames
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    v.refCount = 1;
    list *l = (list *) OUTPUT.auxil;
    // Escape spaces in the pattern, so that wordexp() does not split it into several words
    ppl_strEscapeSpaces(raw, escaped, FNAME_LENGTH);
    if (wordexp(escaped, &w, 0) != 0) return; // No matches; return empty list
    // Expand the wildcards in each word, and add each match to the list
    for (int i = 0; i < w.we_wordc; i++) {
        if (glob(w.we_wordv[i], 0, NULL, &g) != 0) continue;
        // Add each matching filename
        for (int j = 0; j < g.gl_pathc; j++) {
            if ((tmp = ppl_funcStrdup(&call, g.gl_pathv[j])) == NULL) return;
            pplObjStr(&v, 1, 1, tmp);
            ppl_listAppendCpy(l, &v, sizeof(v));
        }
        globfree(&g);
    }
    wordfree(&w);
    return;
}

//! pplfunc_osPopen - Implement the function os.popen(x[,y]) in the os module, which opens a pipe to the command x with
//! access mode y, and returns a file object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osPopen(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *mode;
    // The arguments are the command, and optionally the mode ("r" or "w")
    if ((nArgs < 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The open() function requires string arguments.");
        return;
    }
    // The mode must be a string
    if ((nArgs > 1) && (in[1].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The open() function requires string arguments.");
        return;
    }
    if (nArgs > 1) mode = (char *) in[1].auxil;
    else mode = "r";
    FILE *f = popen((char *) in[0].auxil, mode);
    // Report a failure to start the command
    if (f == NULL) {
        ppl_funcError(status, errType, errText, ERR_FILE, "%s", strerror(errno));
        return;
    }
    pplObjFile(&OUTPUT, 0, 1, f, 1);
}

//! pplfunc_osStat - Implement the function os.stat(x) in the os module, which returns a dictionary of information about
//! the file x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osStat(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // permissions is filled in like the output of ls -l
    pplFuncCall call = {c, in, status, errType, errText, "pplfunc_osStat()"};
    char permissions[] = "---------";
    pplObj v;
    struct stat s;
    // The argument must be a single string, the filename
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE, "The os.stat() function requires a single string argument.");
        return;
    }
    // A file which cannot be examined returns null
    if (stat((char *) in[0].auxil, &s) != 0) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    v.refCount = 1;
    char *tmp = (char *) malloc(LSTR_LENGTH);
    // The output is a dictionary of the file's properties
    if (pplObjDict(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    dict *d = (dict *) OUTPUT.auxil;
    pplObjDate(&v, 1, s.st_atime);
    ppl_dictAppendCpy(d, "atime", &v, sizeof(v));
    pplObjNum(&v, 1, s.st_gid, 0);
    ppl_dictAppendCpy(d, "gid", &v, sizeof(v));
    pplObjDate(&v, 1, s.st_ctime);
    ppl_dictAppendCpy(d, "ctime", &v, sizeof(v));
    pplObjNum(&v, 1, s.st_ino, 0);
    ppl_dictAppendCpy(d, "ino", &v, sizeof(v));
    pplObjNum(&v, 1, s.st_mode, 0);
    ppl_dictAppendCpy(d, "mode", &v, sizeof(v));
    pplObjDate(&v, 1, s.st_mtime);
    ppl_dictAppendCpy(d, "mtime", &v, sizeof(v));
    pplObjNum(&v, 1, s.st_nlink, 0);
    ppl_dictAppendCpy(d, "nlink", &v, sizeof(v));
    pplObjNum(&v, 1, s.st_size * 8, 0);
    v.exponent[UNIT_BIT] = 1;
    v.dimensionless = 0;
    ppl_dictAppendCpy(d, "size", &v, sizeof(v));
    pplObjNum(&v, 1, s.st_uid, 0);
    ppl_dictAppendCpy(d, "uid", &v, sizeof(v));
    // Describe the type of the file
    if (S_ISREG(s.st_mode)) {
        if ((tmp = ppl_funcStrdup(&call, "regular file")) == NULL) return;
    } else if (S_ISDIR(s.st_mode)) {
        if ((tmp = ppl_funcStrdup(&call, "directory")) == NULL) return;
    } else if (S_ISCHR(s.st_mode)) {
        if ((tmp = ppl_funcStrdup(&call, "character special")) == NULL) return;
    } else if (S_ISBLK(s.st_mode)) {
        if ((tmp = ppl_funcStrdup(&call, "block special")) == NULL) return;
    } else if (S_ISFIFO(s.st_mode)) {
        if ((tmp = ppl_funcStrdup(&call, "fifo")) == NULL) return;
    } else if (S_ISLNK(s.st_mode)) {
        if ((tmp = ppl_funcStrdup(&call, "symbolic link")) == NULL) return;
    } else if (S_ISSOCK(s.st_mode)) {
        if ((tmp = ppl_funcStrdup(&call, "socket")) == NULL) return;
    } else {
        if ((tmp = ppl_funcStrdup(&call, "other")) == NULL) return;
    }
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "type", &v, sizeof(v));
    if (s.st_mode & S_IRUSR) permissions[0] = 'r';
    if (s.st_mode & S_IWUSR) permissions[1] = 'w';
    if (s.st_mode & S_IXUSR) permissions[2] = 'x';
    if (s.st_mode & S_IRGRP) permissions[3] = 'r';
    if (s.st_mode & S_IWGRP) permissions[4] = 'w';
    if (s.st_mode & S_IXGRP) permissions[5] = 'x';
    if (s.st_mode & S_IROTH) permissions[6] = 'r';
    if (s.st_mode & S_IWOTH) permissions[7] = 'w';
    if (s.st_mode & S_IXOTH) permissions[8] = 'x';
    if ((tmp = ppl_funcStrdup(&call, permissions)) == NULL) return;
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "permissions", &v, sizeof(v));
    return;
}

//! pplfunc_osSystem - Implement the function os.system(x) in the os module, which executes a command in a subshell
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osSystem(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The argument must be a single string, the command
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The os.system() function requires a single string argument.");
        return;
    }
    pplObjNum(&OUTPUT, 0, system((char *) in[0].auxil), 0);
}

//! pplfunc_osTmpfile - Implement the function os.tmpfile() in the os module, which returns a file handle for a
//! temporary file
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osTmpfile(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    pplObjFile(&OUTPUT, 0, 1, tmpfile(), 0);
}

//! pplfunc_osUname - Implement the function os.uname() in the os module, which returns a dictionary of information
//! about the operating system
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osUname(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The output is a dictionary of the fields returned by uname()
    pplFuncCall call = {c, in, status, errType, errText, "pplfunc_osUname()"};
    pplObj v;
    struct utsname u;
    // Report a failure of uname()
    if (uname(&u)) {
        ppl_funcError(status, errType, errText, ERR_INTERNAL, "The uname() function failed.");
        return;
    }
    v.refCount = 1;
    char *tmp = (char *) malloc(LSTR_LENGTH);
    // The output is a new dictionary
    if (pplObjDict(&OUTPUT, 0, 1, NULL) == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    dict *d = (dict *) OUTPUT.auxil;
    if ((tmp = ppl_funcStrdup(&call, u.sysname)) == NULL) return;
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "sysname", &v, sizeof(v));
    if ((tmp = ppl_funcStrdup(&call, u.nodename)) == NULL) return;
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "nodename", &v, sizeof(v));
    if ((tmp = ppl_funcStrdup(&call, u.release)) == NULL) return;
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "release", &v, sizeof(v));
    if ((tmp = ppl_funcStrdup(&call, u.version)) == NULL) return;
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "version", &v, sizeof(v));
    if ((tmp = ppl_funcStrdup(&call, u.machine)) == NULL) return;
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "machine", &v, sizeof(v));
    return;
}

// Path operations

//! pplfunc_osPathExists - Implement the function os.path.exists(x) in the os.path module, which returns a boolean flag
//! indicating whether a file with pathname x exists
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osPathExists(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The argument must be a single string, the filename
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The os.path.exists() function requires a single string argument.");
        return;
    }
    pplObjBool(&OUTPUT, 0, !access((char *) in[0].auxil, F_OK));
}

//! pplfunc_osPathFilesize - Implement the function os.path.filesize(x) in the os.path module, which returns the size,
//! in bytes, of the file with pathname x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osPathFilesize(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    struct stat buffer;

    // The argument must be a single string, the filename
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The os.path.filesize() function requires a single string argument.");
        return;
    }
    int st = stat((char *) in[0].auxil, &buffer);
    // A file which cannot be examined returns null; otherwise return its size, in bits
    if (st) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    pplObjNum(&OUTPUT, 0, buffer.st_size * 8, 0);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_BIT);
}

//! pplfunc_osPathATime - Implement the function os.path.atime(x) in the os.path module, which returns a date object
//! representing the time of the last access of the file with pathname x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osPathATime(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    struct stat buffer;

    // The argument must be a single string, the filename
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The os.path.atime() function requires a single string argument.");
        return;
    }
    int st = stat((char *) in[0].auxil, &buffer);
    // A file which cannot be examined returns null; otherwise return the time it was last accessed
    if (st) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    pplObjDate(&OUTPUT, 0, buffer.st_atime);
}

//! pplfunc_osPathCTime - Implement the function os.path.ctime(x) in the os.path module, which returns a date object
//! representing the time of the last status change to the file with pathname x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osPathCTime(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    struct stat buffer;

    // The argument must be a single string, the filename
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The os.path.ctime() function requires a single string argument.");
        return;
    }
    int st = stat((char *) in[0].auxil, &buffer);
    // A file which cannot be examined returns null; otherwise return the time its status last changed
    if (st) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    pplObjDate(&OUTPUT, 0, buffer.st_ctime);
}

//! pplfunc_osPathMTime - Implement the function os.path.mtime(x) in the os.path module, which returns a date object
//! representing the time of the last modification of the file with pathname x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osPathMTime(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    struct stat buffer;

    // The argument must be a single string, the filename
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The os.path.mtime() function requires a single string argument.");
        return;
    }
    int st = stat((char *) in[0].auxil, &buffer);
    // A file which cannot be examined returns null; otherwise return the time it was last modified
    if (st) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    pplObjDate(&OUTPUT, 0, buffer.st_mtime);
}

//! pplfunc_osPathExpandUser - Implement the function os.path.expanduser(x) in the os.path module, which returns its
//! argument with ~s indicating home directories expanded
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osPathExpandUser(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const int outlen = LSTR_LENGTH;
    // The argument must be a single string, the filename
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The os.path.expanduser() function requires a single string argument.");
        return;
    }
    char *out = (char *) malloc(outlen);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    ppl_unixExpandUserHomeDir(&c->errcontext, (char *) in[0].auxil, c->errcontext.session_default.cwd, out, outlen);
    pplObjStr(&OUTPUT, 0, 1, out);
    return;
}

//! osPathJoinAppend - Append one component to a pathname being built by os.path.join(), enlarging the buffer as needed
//! \param [in,out] out - Pointer to the malloced buffer holding the pathname
//! \param [in,out] outLen - The size of the buffer <*out>
//! \param [in,out] j - The length of the pathname so far
//! \param [in] item - The component to append
//! \param [in] first - True if this is the first component, which is not preceded by a separator
//! \return Zero on success, or nonzero if memory ran out (in which case <*out> is freed)

static int osPathJoinAppend(char **out, int *outLen, int *j, const char *item, const int first) {
    const int itemLen = (int) strlen(item);
    const int sepLen = (int) strlen(PATHLINK);
    // Separate the item from what came before it with a /
    if (!first) {
        // Absolute paths replace whatever came before them
        if (strncmp(item, PATHLINK, sepLen) == 0) *j = 0;
        // Append the separator
        else {
            memcpy(*out + *j, PATHLINK, sepLen + 1);
            *j += sepLen;
        }
    }
    // Grow the buffer if the item will not fit
    if (*j + itemLen + sepLen + 1 > *outLen) {
        const int newLen = *j + itemLen + sepLen + FNAME_LENGTH;
        char *newOut = (char *) realloc(*out, newLen);
        // Failure to allocate memory frees the buffer, which the caller detects
        if (newOut == NULL) {
            free(*out);
            *out = NULL;
            return 1;
        }
        *out = newOut;
        *outLen = newLen;
    }
    memcpy(*out + *j, item, itemLen + 1);
    *j += itemLen;
    while ((*j > 0) && ((*out)[*j - 1] == PATHLINK[0])) (*out)[--(*j)] = '\0'; // Remove trailing separators
    return 0;
}

//! pplfunc_osPathJoin - Implement the function os.path.join(...) in the os.path module, which joins a series of strings
//! intelligently into a pathname
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osPathJoin(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The path is built up in out, of length outLen, with j characters used so far
    int outLen = FNAME_LENGTH, j = 0, i = 0;
    if (nArgs < 1) return;
    // The arguments are either a single list of strings, or one or more strings
    if ((in[0].objType == PPLOBJ_LIST) && (nArgs > 1)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The os.path.join() function must be called with a single list of strings.");
        return;
    }
    char *out = (char *) malloc(outLen);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    out[0] = '\0';

    // Join the items of a list
    if (in[0].objType == PPLOBJ_LIST) {
        // A single list of strings
        listIterator *iter = ppl_listIterateInit((list *) in[0].auxil);
        pplObj *item;
        // Append each item, which must be a string
        while ((item = (pplObj *) ppl_listIterate(&iter)) != NULL) {
            // Report an item which is not a string
            if (item->objType != PPLOBJ_STR) {
                ppl_funcError(status, errType, errText, ERR_TYPE,
                        "The os.path.join() function must be passed a list of string; input %d has type <%s>.", i + 1,
                        pplObjTypeNames[item->objType]);
                free(out);
                out = NULL;
                return;
            }
            if (osPathJoinAppend(&out, &outLen, &j, (char *) item->auxil, i == 0)) break;
            i++;
        }
    } else {
        // A series of string arguments
        for (i = 0; i < nArgs; i++) {
            // Every argument must be a string
            if (in[i].objType != PPLOBJ_STR) {
                ppl_funcError(status, errType, errText, ERR_TYPE,
                        "The os.path.join() function must be passed a list of string; input %d has type <%s>.", i + 1,
                        pplObjTypeNames[in[i].objType]);
                free(out);
                out = NULL;
                return;
            }
            if (osPathJoinAppend(&out, &outLen, &j, (char *) in[i].auxil, i == 0)) break;
        }
    }
    // A failure to allocate memory while joining
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "Out of memory.");
        return;
    }
    pplObjStr(&OUTPUT, 0, 1, out);
}
