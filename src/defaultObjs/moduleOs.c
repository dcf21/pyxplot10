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
#include "defaultObjs/defaultFuncsMacros.h"

#define COPYSTR(X, Y) \
    { \
        X = (char *) malloc(strlen(Y) + 1); \
        if (X == NULL) { \
            *status = 1; \
            *errType = ERR_MEMORY; \
            ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory."); \
            return; \
        } \
        strcpy(X, Y); \
    }

//! pplfunc_osChdir - Implement the function os.chdir(x) in the os module, which changes working directory to x
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_osChdir(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.chdir() function requires a single string argument.");
        return;
    }
    if (chdir((char *) in[0].auxil) != 0) {
        *status = 1;
        *errType = ERR_FILE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.chdir() encountered an error: %s", strerror(errno));
        return;
    }
    strncpy(c->errcontext.session_default.cwd, (char *) in[0].auxil, FNAME_LENGTH);
    c->errcontext.session_default.cwd[FNAME_LENGTH - 1] = '\0';
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
    char *tmp;
    COPYSTR(tmp, c->errcontext.session_default.cwd);
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
    char *out, *tmp;
    tmp = ppl_unixGetHomeDir(&c->errcontext);
    out = (char *) malloc(strlen(tmp) + 1);
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    char *out;
    out = (char *) malloc(outlen);
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    char *out, *tmp;
    tmp = getlogin();
    out = (char *) malloc(strlen(tmp) + 1);
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    char *out, *tmp;
    tmp = ppl_unixGetIRLName(&c->errcontext);
    out = (char *) malloc(strlen(tmp) + 1);
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    wordexp_t w;
    glob_t g;
    pplObj v;
    list *l;
    char *tmp;
    char escaped[FNAME_LENGTH], *raw = (char *) in[0].auxil;
    int i, j;
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.glob() function requires a single string argument.");
        return;
    }
    if (pplObjList(&OUTPUT, 0, 1, NULL) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    v.refCount = 1;
    l = (list *) OUTPUT.auxil;
    {
        int j, k;
        for (j = k = 0; ((raw[j] != '\0') && (k < FNAME_LENGTH - 1));) {
            if (raw[j] == ' ') escaped[k++] = '\\';
            escaped[k++] = raw[j++];
        }
        escaped[k++] = '\0';
    }
    if (wordexp(escaped, &w, 0) != 0) return; // No matches; return empty list
    for (i = 0; i < w.we_wordc; i++) {
        if (glob(w.we_wordv[i], 0, NULL, &g) != 0) continue;
        for (j = 0; j < g.gl_pathc; j++) {
            COPYSTR(tmp, g.gl_pathv[j]);
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
    FILE *f;
    char *mode;
    if ((nArgs < 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The open() function requires string arguments.");
        return;
    }
    if ((nArgs > 1) && (in[1].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The open() function requires string arguments.");
        return;
    }
    if (nArgs > 1) mode = (char *) in[1].auxil;
    else mode = "r";
    f = popen((char *) in[0].auxil, mode);
    if (f == NULL) {
        *status = 1;
        *errType = ERR_FILE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "%s", strerror(errno));
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
    char *tmp, permissions[] = "---------";
    dict *d;
    pplObj v;
    struct stat s;
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.stat() function requires a single string argument.");
        return;
    }
    if (stat((char *) in[0].auxil, &s) != 0) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    v.refCount = 1;
    tmp = (char *) malloc(LSTR_LENGTH);
    if (pplObjDict(&OUTPUT, 0, 1, NULL) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    d = (dict *) OUTPUT.auxil;
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
    if (S_ISREG(s.st_mode)) {
        COPYSTR(tmp, "regular file");
    } else if (S_ISDIR(s.st_mode)) {
        COPYSTR(tmp, "directory");
    } else if (S_ISCHR(s.st_mode)) {
        COPYSTR(tmp, "character special");
    } else if (S_ISBLK(s.st_mode)) {
        COPYSTR(tmp, "block special");
    } else if (S_ISFIFO(s.st_mode)) {
        COPYSTR(tmp, "fifo");
    } else if (S_ISLNK(s.st_mode)) {
        COPYSTR(tmp, "symbolic link");
    } else if (S_ISSOCK(s.st_mode)) {
        COPYSTR(tmp, "socket");
    } else {
        COPYSTR(tmp, "other");
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
    COPYSTR(tmp, permissions);
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
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.system() function requires a single string argument.");
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
    char *tmp;
    dict *d;
    pplObj v;
    struct utsname u;
    if (uname(&u)) {
        *status = 1;
        *errType = ERR_INTERNAL;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The uname() function failed.");
        return;
    }
    v.refCount = 1;
    tmp = (char *) malloc(LSTR_LENGTH);
    if (pplObjDict(&OUTPUT, 0, 1, NULL) == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    d = (dict *) OUTPUT.auxil;
    COPYSTR(tmp, u.sysname);
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "sysname", &v, sizeof(v));
    COPYSTR(tmp, u.nodename);
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "nodename", &v, sizeof(v));
    COPYSTR(tmp, u.release);
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "release", &v, sizeof(v));
    COPYSTR(tmp, u.version);
    pplObjStr(&v, 1, 1, tmp);
    ppl_dictAppendCpy(d, "version", &v, sizeof(v));
    COPYSTR(tmp, u.machine);
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
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.path.exists() function requires a single string argument.");
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
    int st;

    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.path.filesize() function requires a single string argument.");
        return;
    }
    st = stat((char *) in[0].auxil, &buffer);
    if (st) {
        pplObjNull(&OUTPUT, 0);
        return;
    }
    pplObjNum(&OUTPUT, 0, buffer.st_size * 8, 0);
    CLEANUP_APPLYUNIT(UNIT_BIT);
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
    int st;

    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.path.atime() function requires a single string argument.");
        return;
    }
    st = stat((char *) in[0].auxil, &buffer);
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
    int st;

    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.path.ctime() function requires a single string argument.");
        return;
    }
    st = stat((char *) in[0].auxil, &buffer);
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
    int st;

    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.path.mtime() function requires a single string argument.");
        return;
    }
    st = stat((char *) in[0].auxil, &buffer);
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
    char *out;
    if ((nArgs != 1) || (in[0].objType != PPLOBJ_STR)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "The os.path.expanduser() function requires a single string argument.");
        return;
    }
    out = (char *) malloc(outlen);
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
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
    if (!first) {
        // Absolute paths replace whatever came before them
        if (strncmp(item, PATHLINK, sepLen) == 0) *j = 0;
        else {
            memcpy(*out + *j, PATHLINK, sepLen + 1);
            *j += sepLen;
        }
    }
    if (*j + itemLen + sepLen + 1 > *outLen) {
        const int newLen = *j + itemLen + sepLen + FNAME_LENGTH;
        char *newOut = (char *) realloc(*out, newLen);
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
    int outLen = FNAME_LENGTH, j = 0, i = 0;
    char *out;
    if (nArgs < 1) return;
    if ((in[0].objType == PPLOBJ_LIST) && (nArgs > 1)) {
        *status = 1;
        *errType = ERR_TYPE;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "The os.path.join() function must be called with a single list of strings.");
        return;
    }
    out = (char *) malloc(outLen);
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    out[0] = '\0';

    if (in[0].objType == PPLOBJ_LIST) {
        // A single list of strings
        listIterator *iter = ppl_listIterateInit((list *) in[0].auxil);
        pplObj *item;
        while ((item = (pplObj *) ppl_listIterate(&iter)) != NULL) {
            if (item->objType != PPLOBJ_STR) {
                *status = 1;
                *errType = ERR_TYPE;
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
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
            if (in[i].objType != PPLOBJ_STR) {
                *status = 1;
                *errType = ERR_TYPE;
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "The os.path.join() function must be passed a list of string; input %d has type <%s>.", i + 1,
                        pplObjTypeNames[in[i].objType]);
                free(out);
                out = NULL;
                return;
            }
            if (osPathJoinAppend(&out, &outLen, &j, (char *) in[i].auxil, i == 0)) break;
        }
    }
    if (out == NULL) {
        *status = 1;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Out of memory.");
        return;
    }
    pplObjStr(&OUTPUT, 0, 1, out);
}
