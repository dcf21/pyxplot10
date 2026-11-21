// core.c
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

// Implements Pyxplot's simpler commands: assert, cd, exec, global, history, load, local, print and save, the error
// messages for unrecognised set and unset options, and variable assignment, including the =~ operator, which applies a
// sed regular expression to a string variable by piping it through a child sed process.

#define _CORE_C 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <wordexp.h>
#include <glob.h>
#include <math.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>

#ifdef HAVE_READLINE
#include <readline/history.h>
#endif

#include <gsl/gsl_math.h>

#include "commands/core.h"

#include "coreUtils/backup.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/getPasswd.h"

#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"

#include "settings/settingTypes.h"
#include "settings/textConstants.h"

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "parser/cmdList.h"
#include "parser/parser.h"

#include "expressions/expEval.h"
#include "expressions/traceback.h"

#include "userspace/context.h"
#include "userspace/contextVarDef.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjPrint.h"

#include "children.h"
#include "input.h"
#include "pplConstants.h"


//! versionError - Report an error in the version clause of an assert command
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command
//! \param [in] in - The parser output for the command
//! \param [in] errType - The type of error, one of the ERR_* constants
//! \param [in] msg - The error message

static void versionError(ppl_context *c, const parserLine *pl, const parserOutput *in, const int errType,
                         const char *msg) {
    ppl_snprintfTrunc(c->errStat.errBuff, LSTR_LENGTH, "%s", msg);
    ppl_tbAddLine(c, pl, errType, in->stkCharPos[PARSE_assert_version], "");
}

//! ppl_directive_assert - Implement the assert command, which raises an error if an expression is false, or if a
//! version requirement is not met
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters

void ppl_directive_assert(ppl_context *c, const parserLine *pl, const parserOutput *in) {
    // Read the parameters of the command
    pplObj *stk = in->stk;

    // The optional message, version requirement and expression
    char *txt = (stk[PARSE_assert_message].objType == PPLOBJ_STR) ? (char *) stk[PARSE_assert_message].auxil : NULL;
    char *version = (stk[PARSE_assert_version].objType == PPLOBJ_STR) ? (char *) stk[PARSE_assert_version].auxil : NULL;
    pplObj *val = &stk[PARSE_assert_expr];
    const int lt = (stk[PARSE_assert_lt].objType == PPLOBJ_STR);

    // If an expression was given, it must be true
    if (val->objType != PPLOBJ_ZOM) {
        ppl_castToBool(c, val);
        if (txt == NULL) txt = "Assertion was not true.";
        // Report a false assertion
        if (val->real < 0.5) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "%s", txt);
            ppl_tbAddLine(c, pl, ERR_ASSERT, in->stkCharPos[PARSE_assert_expr], "");
            return;
        }
    }

    // If a version requirement was given, compare it with this version of Pyxplot, one component at a time; sgn
    // reverses the comparisons for a requirement that the version be older
    if (version != NULL) {
        int i = 0, j = 0, pass = 0;
        char txtauto[64];
        ppl_snprintfTrunc(txtauto, sizeof(txtauto), "This script requires a%s version of Pyxplot (%s %s)",
                          (!lt) ? " newer" : "n older", (!lt) ? ">=" : "<", version);
        if (txt == NULL) txt = txtauto;
        int sgn = (!lt) ? 1 : -1;
        while ((version[i] > '\0') && ((unsigned char) version[i] <= ' ')) i++;
        if (version[i] == '\0') return;
        // Read the major version number required
        for (j = 0; ((version[i] >= '0') && (version[i] <= '9')); i++) {
            j = 10 * j + (version[i] - '0');
            if (j > 1e6) j = 1e6;
        }
        // Fail if the major version does not meet the requirement
        if ((sgn * VERSION_MAJ < sgn * j) && (!pass)) {
            versionError(c, pl, in, ERR_ASSERT, txt);
            return;
        }
        if (sgn * VERSION_MAJ > sgn * j) pass = 1;
        while ((version[i] > '\0') && ((unsigned char) version[i] <= ' ')) i++;
        if (version[i] == '.') i++;
        // Anything other than a dot after the number is malformed
        else if (version[i] != '\0') {
            {
                versionError(c, pl, in, ERR_SYNTAX, "Malformed version string.");
                return;
            }
        }
        while ((version[i] > '\0') && ((unsigned char) version[i] <= ' ')) i++;
        // A strict "older than" requirement fails if the major versions are equal and no minor version was given
        if ((version[i] == '\0') && (sgn == -1) && (!pass)) {
            versionError(c, pl, in, ERR_ASSERT, txt);
            return;
        }
        if (version[i] == '\0') return;
        // Read the minor version number required
        for (j = 0; ((version[i] >= '0') && (version[i] <= '9')); i++) {
            j = 10 * j + (version[i] - '0');
            if (j > 1e6) j = 1e6;
        }
        // Fail if the minor version does not meet the requirement
        if ((sgn * VERSION_MIN < sgn * j) && (!pass)) {
            versionError(c, pl, in, ERR_ASSERT, txt);
            return;
        }
        if (sgn * VERSION_MIN > sgn * j) pass = 1;
        while ((version[i] > '\0') && ((unsigned char) version[i] <= ' ')) i++;
        if (version[i] == '.') i++;
        // Anything other than a dot after the number is malformed
        else if (version[i] != '\0') {
            {
                versionError(c, pl, in, ERR_SYNTAX, "Malformed version string.");
                return;
            }
        }
        while ((version[i] > '\0') && ((unsigned char) version[i] <= ' ')) i++;
        // A strict "older than" requirement fails if the versions are equal so far and no revision was given
        if ((version[i] == '\0') && (sgn == -1) && (!pass)) {
            versionError(c, pl, in, ERR_ASSERT, txt);
            return;
        }
        if (version[i] == '\0') return;
        // Read the revision number required
        for (j = 0; ((version[i] >= '0') && (version[i] <= '9')); i++) {
            j = 10 * j + (version[i] - '0');
            if (j > 1e6) j = 1e6;
        }
        // Fail if the revision does not meet the requirement
        if ((sgn * VERSION_REV < sgn * j) && (!pass)) {
            versionError(c, pl, in, ERR_ASSERT, txt);
            return;
        }
        if (sgn * VERSION_REV > sgn * j) pass = 1;
        while ((version[i] > '\0') && ((unsigned char) version[i] <= ' ')) i++;
        // Nothing may follow the revision number
        if (version[i] != '\0') {
            {
                versionError(c, pl, in, ERR_SYNTAX, "Malformed version string.");
                return;
            }
        }
        // A strict "older than" requirement fails if the versions are equal
        if ((version[i] == '\0') && (sgn == -1) && (!pass)) {
            versionError(c, pl, in, ERR_ASSERT, txt);
            return;
        }
    }

    // The assertion passed
    return;
}

//! ppl_directive_cd - Implement the cd command, which changes the current working directory
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters

void ppl_directive_cd(ppl_context *c, const parserLine *pl, const parserOutput *in) {
    // The path is a list of directory names, which are entered in turn
    pplObj *stk = in->stk;
    int pos = PARSE_cd_path;
    wordexp_t wordExp;
    glob_t globData;

    // Walk the linked list of directory names on the parser stack
    while (stk[pos].objType == PPLOBJ_NUM) {
        char dirName[FNAME_LENGTH];
        pos = (int) round(stk[pos].real);
        if (pos <= 0) break;
        char *raw = (char *) stk[pos + PARSE_cd_directory_path].auxil;
        // Escape spaces in the name before passing it to wordexp
        ppl_strEscapeSpaces(raw, dirName, FNAME_LENGTH);

        // Expand ~ and environment variables in the name
        if ((wordexp(dirName, &wordExp, 0) != 0) || (wordExp.we_wordc <= 0)) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "Could not enter directory '%s'.", dirName);
            ppl_tbAddLine(c, pl, ERR_FILE, in->stkCharPos[pos + PARSE_cd_directory_path], "");
            return;
        }
        // Expand wildcards in the name
        if ((glob(wordExp.we_wordv[0], 0, NULL, &globData) != 0) || (globData.gl_pathc <= 0)) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "Could not enter directory '%s'.", dirName);
            ppl_tbAddLine(c, pl, ERR_FILE, in->stkCharPos[pos + PARSE_cd_directory_path], "");
            wordfree(&wordExp);
            return;
        }
        wordfree(&wordExp);
        // Enter the directory
        if (chdir(globData.gl_pathv[0]) < 0) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "Could not change into directory '%s'.", globData.gl_pathv[0]);
            ppl_tbAddLine(c, pl, ERR_FILE, in->stkCharPos[pos + PARSE_cd_directory_path], "");
            globfree(&globData);
            break;
        }
        globfree(&globData);
    }
    if (getcwd(c->errcontext.session_default.cwd, FNAME_LENGTH) == NULL) {
        ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Could not read current working directory.");
    } // Store cwd
    return;
}

//! ppl_directive_exec - Implement the exec command, which executes a string as a Pyxplot command
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_exec(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                        const int iterDepth) {
    pplObj *stk = in->stk;
    char *cmd = (char *) stk[PARSE_exec_command].auxil;
    parserLine *pl2 = NULL;
    parserStatus *ps2 = NULL;

    // A new parser state, so that the command can contain blocks of its own
    ppl_parserStatInit(&ps2, &pl2);
    // Failure to allocate memory is reported as an error
    if ((ps2 == NULL) || (c->inputLineBuffer == NULL)) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
        return;
    }
    ppl_error_setstreaminfo(&c->errcontext, -1, "executed statement");

    // Execute the string as if it had been typed
    ppl_processLine(c, ps2, cmd, interactive, iterDepth + 1);
    // Add this command to the traceback of any error
    if (c->errStat.status) {
        strcpy(c->errStat.errBuff, "");
        ppl_tbAdd(c, pl->srcLineN, pl->srcId, pl->srcFname, 0, ERR_GENERIC, 0, pl->linetxt, "executed statement");
    }
    // Warn about a string which ends with a line continuation character
    if (c->inputLineAddBuffer != NULL) {
        ppl_warning(&c->errcontext, ERR_SYNTAX,
                    "Line continuation character (\\) at the end of executed string, with no line following it.");
        free(c->inputLineAddBuffer);
        c->inputLineAddBuffer = NULL;
    }
    ppl_parserStatFree(&ps2);
    return;
}

//! ppl_directive_global - Implement the global command, which declares that variables referred to within a
//! subroutine should refer to global variables, rather than local ones
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_global(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                          const int iterDepth) {
    pplObj *stk = in->stk;

    // Global variables can only be declared inside a subroutine or module
    if (c->ns_ptr < 2) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Cannot declare global variables when not in a subroutine or module namespace.");
        ppl_tbAddLine(c, pl, ERR_NAMESPACE, 0, "");
        return;
    }

    // Declare each variable named as global
    int pos = PARSE_global_var_names;
    // Walk the linked list of variable names on the parser stack
    while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {

        // Look up the variable in the local namespace
        pos = (int) round(stk[pos].real);
        char *varname = (char *) stk[pos + PARSE_global_var_name_var_names].auxil;
        pplObj *obj = (pplObj *) ppl_dictLookup(c->namespaces[c->ns_ptr], varname);
        // If it does not exist locally, create it
        if ((obj == NULL) && (!c->namespaces[c->ns_ptr]->immutable)) {
            pplObj out;
            pplObjZom(&out, 1);
            out.refCount = 1; // Create a temporary zombie for now
            ppl_dictAppendCpy(c->namespaces[c->ns_ptr], varname, (void *) &out, sizeof(pplObj));
            obj = (pplObj *) ppl_dictLookup(c->namespaces[c->ns_ptr], varname);
            // Failure to allocate memory is reported as an error
            if (obj == NULL) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
                ppl_tbAddLine(c, pl, ERR_MEMORY, 0, "");
                return;
            }
        }
        // The local namespace may not be immutable
        if ((obj == NULL) || (c->namespaces[c->ns_ptr]->immutable)) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                              "Cannot modify variable in immutable namespace.");
            ppl_tbAddLine(c, pl, ERR_NAMESPACE, in->stkCharPos[pos + PARSE_global_var_name_var_names], "");
            return;
        }
        if (obj->objType == PPLOBJ_GLOB) return;
        const int om = obj->amMalloced;
        const int rc = obj->refCount;
        obj->amMalloced = 0;
        obj->refCount = 1;
        ppl_garbageObject(obj);
        pplObjGlobal(obj, om);
        obj->refCount = rc;
    }
    return;
}

//! ppl_directive_history - Implement the history command, which displays the commands typed in the current
//! interactive session
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters

void ppl_directive_history(ppl_context *c, const parserLine *pl, const parserOutput *in) {
#ifdef HAVE_READLINE
    pplObj *stk = in->stk;
    int start = 0;
    HIST_ENTRY **history_data;

    // Fetch readline's history
    const int endpos = where_history();
    history_data = history_list();

    // Show only the requested number of lines, if specified
    if (stk[PARSE_history_number_lines].objType == PPLOBJ_NUM) {
        start = endpos - (int) round(stk[PARSE_history_number_lines].real);
        if (start < 0) start = 0;
    }

    // Print each line of history
    for (int k = start; k < endpos; k++) ppl_report(&c->errcontext, history_data[k]->line);
    return;
#else
    snprintf(
            c->errStat.errBuff, LSTR_LENGTH,
            "The 'history' command is not available as the GNU readline library was not linked to when Pyxplot was installed.");
    ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
    return;
#endif
}

//! ppl_directive_load - Implement the load command, which executes the commands in a script file, or in all files
//! matching a wildcard
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_load(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                        const int iterDepth) {
    int i = 0;
    int j = 0;
    pplObj *stk = in->stk;
    char *raw = (char *) stk[PARSE_load_filename].auxil;
    char fn[FNAME_LENGTH];
    wordexp_t wordExp;
    glob_t globData;
    // Escape spaces in the filename before passing it to wordexp
    ppl_strEscapeSpaces(raw, fn, FNAME_LENGTH);


    // Expand ~ and environment variables in the filename
    if ((wordexp(fn, &wordExp, 0) != 0) || (wordExp.we_wordc <= 0)) {
        snprintf(c->errStat.errBuff, LSTR_LENGTH, "Could not access file '%s'.", fn);
        ppl_tbAddLine(c, pl, ERR_FILE, in->stkCharPos[PARSE_load_filename], "");
        return;
    }
    // Run every script matching each word of the expanded filename, stopping if the user presses control-C
    for (j = 0; (j < wordExp.we_wordc) && (!cancellationFlag); j++) {
        // Expand wildcards; report an error if nothing matches and only one name was given
        if ((glob(wordExp.we_wordv[j], 0, NULL, &globData) != 0) || (globData.gl_pathc <= 0)) {
            // Report an error
            if (wordExp.we_wordc == 1) {
                snprintf(c->errStat.errBuff, LSTR_LENGTH, "Could not access file '%s'.", fn);
                ppl_tbAddLine(c, pl, ERR_FILE, in->stkCharPos[PARSE_load_filename], "");
                wordfree(&wordExp);
                return;
            } else {
                continue;
            }
        }
        // Run each matching script
        for (i = 0; (i < globData.gl_pathc) && (!cancellationFlag); i++) {
            ppl_processScript(c, globData.gl_pathv[i], iterDepth + 1);
            // Add this command to the traceback of any error
            if (c->errStat.status) {
                strcpy(c->errStat.errBuff, "");
                ppl_tbAdd(c, pl->srcLineN, pl->srcId, pl->srcFname, 0, ERR_GENERIC, 0, pl->linetxt, "executed script");
                break;
            }
        }
        globfree(&globData);
    }
    wordfree(&wordExp);
    return;
}

//! ppl_directive_local - Implement the local command, which declares that variables referred to within a subroutine
//! should be local to it
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_local(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                         const int iterDepth) {
    pplObj *stk = in->stk;

    // Local variables can only be declared inside a subroutine or module
    if (c->ns_ptr < 2) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                          "Cannot declare local variables when not in a subroutine or module namespace.");
        ppl_tbAddLine(c, pl, ERR_NAMESPACE, 0, "");
        return;
    }

    // Make each variable named local again
    int pos = PARSE_local_var_names;
    // Walk the linked list of variable names on the parser stack
    while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {
        pplObj *obj;

        // Look up the variable
        pos = (int) round(stk[pos].real);
        char *varname = (char *) stk[pos + PARSE_local_var_name_var_names].auxil;

        // Look up requested variable
        ppl_contextVarLookup(c, varname, &obj, 1);
        // The variable must previously have been declared global
        if ((obj == NULL) || (obj->objType != PPLOBJ_GLOB)) {
            ppl_snprintfTrunc(
                    c->errStat.errBuff, sizeof(c->errStat.errBuff),
                    "Variable '%s' has not previously been declared global in this namespace. It is already a local variable.",
                    varname);
            ppl_tbAddLine(c, pl, ERR_NAMESPACE, in->stkCharPos[pos + PARSE_local_var_name_var_names], "");
            return;
        }

        // Replace the global marker with an undefined local variable
        pplObjZom(obj, obj->amMalloced);
    }
    return;
}

//! ppl_directive_print - Implement the print command, which displays the values of a list of expressions
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters

void ppl_directive_print(ppl_context *c, const parserLine *pl, const parserOutput *in) {
    // The values are printed one after another into a single buffer
    char msg[LSTR_LENGTH];
    pplObj *stk = in->stk;
    int pos = PARSE_print_0print_list;
    int i = 0;
    int gotItems = 0;
    msg[i] = '\0';

    // Walk the linked list of the expressions on the parser stack, printing each value
    while (stk[pos].objType == PPLOBJ_NUM) {
        pos = (int) round(stk[pos].real);
        if (pos <= 0) break;
        pplObjPrint(c, stk + pos + PARSE_print_expression_0print_list, NULL, msg + i, sizeof(msg) - i, 0, 0);
        msg[sizeof(msg) - 1] = '\0';
        i += strlen(msg + i);
        gotItems = 1;
    }
    ppl_report(&c->errcontext, msg);

    // set ans
    if (gotItems) {
        pplObj *optr, dummyTemp;
        ppl_contextGetVarPointer(c, "ans", &optr, &dummyTemp);
        dummyTemp.amMalloced = 0;
        ppl_garbageObject(&dummyTemp);
        const int am = optr->amMalloced;
        const int rc = optr->refCount;
        optr->amMalloced = 0;
        ppl_garbageObject(optr);
        pplObjCpy(optr, stk + pos + PARSE_print_expression_0print_list, 0, 0, 1);
        optr->amMalloced = am;
        optr->refCount = rc;
        optr->self_lval = NULL;
        optr->self_this = NULL;
    }
    return;
}

//! ppl_directive_save - Implement the save command, which writes the commands typed in the current interactive
//! session to a file
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters

void ppl_directive_save(ppl_context *c, const parserLine *pl, const parserOutput *in) {
    // Variables for writing the history to a file
    char msg[PPL_MSG_LENGTH];
#ifdef HAVE_READLINE
    pplObj *stk = in->stk;
    const int pos = PARSE_save_filename;
    int start = 0;
    long x;
    char *outfname = "";
    FILE *outfile = NULL;
    HIST_ENTRY **history_data;

    // Open the output file, backing up any file it replaces
    if (stk[pos].objType == PPLOBJ_STR) {
        outfname = (char *) stk[pos].auxil;
        ppl_createBackupIfRequired(c, outfname);
        outfile = fopen(outfname, "w");
    }
    // Report a failure to open the file
    if (outfile == NULL) {
        snprintf(c->errStat.errBuff, LSTR_LENGTH, "The save command could not open output file '%s' for writing.",
                 outfname);
        ppl_tbAddLine(c, pl, ERR_FILE, in->stkCharPos[pos], "");
        return;
    }
    fprintf(outfile, "# Command script saved by Pyxplot %s\n# Timestamp: %s\n", VERSION,
            ppl_strStrip(ppl_friendlyTimestring(), msg));
    fprintf(outfile, "# User: %s\n\n", ppl_unixGetIRLName(&c->errcontext));

    // Fetch readline's history
    const int endpos = history_length;
    history_data = history_list();

    // Save only the lines typed since the last save
    x = endpos - c->historyNLinesWritten;
    if (x < history_base) x = history_base;
    start = (int) x;

    // Write each line, except the last, which is this save command
    for (int k = start; k < endpos - 1; k++) { fprintf(outfile, "%s\n", (history_data[k]->line)); }
    fclose(outfile);
    return;
#else
    snprintf(
            c->errStat.errBuff, LSTR_LENGTH,
            "The 'save' command is not available as the GNU readline library was not linked to when Pyxplot was installed.");
    ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
    return;
#endif
}

//! ppl_directive_seterror - Implement the "set xerrorbars" family of commands, which select a plot style with
//! error bars
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal

void ppl_directive_seterror(ppl_context *c, const parserLine *pl, parserOutput *in, const int interactive) {
    pplObj *o = &in->stk[PARSE_set_error_set_option];
    char *tempstr = (o->objType == PPLOBJ_STR) ? (char *) in->stk[PARSE_set_error_set_option].auxil : NULL;
    // Report the unrecognised option; in interactive sessions, list the options which are available
    if (tempstr != NULL) {
        // In scripts, report an error
        if (!interactive) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "Unrecognised set option '%s'.", tempstr);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[PARSE_set_error_set_option], "");
        } else {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, ppltxt_set, tempstr);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[PARSE_set_error_set_option], "");
        }
    } else {
        // In scripts, report an error
        if (!interactive) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "set command detected with no set option following it.");
            ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
        } else {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "%s", ppltxt_set_noword);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
        }
    }
    return;
}

//! ppl_directive_unseterror - Implement the "unset xerrorbars" family of commands, which revert the default plot
//! style
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal

void ppl_directive_unseterror(ppl_context *c, const parserLine *pl, parserOutput *in, const int interactive) {
    pplObj *o = &in->stk[PARSE_set_error_set_option];
    char *tempstr = (o->objType == PPLOBJ_STR) ? (char *) in->stk[PARSE_set_error_set_option].auxil : NULL;
    // Report the unrecognised option; in interactive sessions, list the options which are available
    if (tempstr != NULL) {
        // In scripts, report an error
        if (!interactive) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "Unrecognised set option '%s'.", tempstr);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[PARSE_set_error_set_option], "");
        } else {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, ppltxt_unset, tempstr);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[PARSE_set_error_set_option], "");
        }
    } else {
        // In scripts, report an error
        if (!interactive) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "unset command detected with no set option following it.");
            ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
        } else {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "%s", ppltxt_unset_noword);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
        }
    }
    return;
}

//! ppl_lookupAssignable - Look up a variable which a command is to assign a value to, which may have a hierarchical
//! name such as a.b, or a subscripted name such as a[2], creating it if necessary
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command
//! \param [in] pos1 - The position in the parser output of the list of the parts of the variable's name
//! \param [in] pos2 - The offset of each part of the name within that list
//! \return The variable, or NULL if an error was reported, including when the variable is a global declaration at
//! the top level

pplObj *ppl_lookupAssignable(ppl_context *c, const parserLine *pl, const parserOutput *in, const int pos1,
                             const int pos2) {
    pplObj *obj = NULL;
    ppl_contextVarHierLookup(c, pl->srcLineN, pl->srcId, pl->srcFname, pl->linetxt, in->stk, in->stkCharPos, &obj, pos1,
                             pos2);
    if ((c->errStat.status) || (obj == NULL)) return NULL;
    // A global declaration at the top level cannot be overwritten
    if (obj->objType == PPLOBJ_GLOB) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Variable declared global in global namespace.");
        ppl_tbAddLine(c, pl, ERR_NAMESPACE, 0, "");
        return NULL;
    }
    return obj;
}

//! ppl_directive_varset - Implement assignments to variables, including hierarchical names such as a.b and
//! subscripted names such as a[2], and the =~ string substitution operator
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_directive_varset(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                          const int iterDepth) {
    pplObj *stk = in->stk;
    pplObj *val = &stk[PARSE_var_set_value];
    pplObj *rgx = &stk[PARSE_var_set_regex];
    pplObj *obj = NULL;

    // Look up the variable to set
    obj = ppl_lookupAssignable(c, pl, in, PARSE_var_set_varnames, PARSE_var_set_varname_varnames);
    if (obj == NULL) return;

    if (rgx->objType != PPLOBJ_STR) // variable assignment. not regular expression
    {
        ppl_objReplace(obj, val);
        obj->immutable = 0;
    } else // regular expression
    {
        int fstdin, fstdout;
        sigset_t sigs;
        fd_set readable;
        char *re = NULL, *outstr = NULL;
        char *in = (char *) rgx->auxil;
        char *instr = (char *) obj->auxil;

        // Check that input is a string
        if (obj->objType != PPLOBJ_STR) {
            ppl_error(&c->errcontext, ERR_TYPE, -1, -1,
                      "The regular expression operator can only act on variables which already have string values.");
            return;
        }

        // Append 's' to the beginning of the regular expression
        int rel = (strlen(in) + 8) * 8;
        re = (char *) malloc(rel);
        // Failure to allocate memory is reported as an error
        if (re == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return;
        }
        sprintf(re, "s%s", in);

        // Block sigchild
        sigemptyset(&sigs);
        sigaddset(&sigs, SIGCHLD);

        // Fork a child sed process with the regular expression on the command line, and send variable contents to it
        pplcsp_forkSed(c, re, &fstdin, &fstdout);
        if (write(fstdin, instr, strlen(instr)) != strlen(instr))
            ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Could not write to pipe to sed");
        close(fstdin);

        // Wait for sed process's stdout to become readable. Get bored if this takes more than a second.
        int trialNumber = 1;
        // Retry the wait up to three times if it is interrupted by a signal
        while (1) {
            struct timespec waitperiod; // A time.h timespec specifier for a wait of zero seconds
            waitperiod.tv_sec = 1;
            waitperiod.tv_nsec = 0;
            FD_ZERO(&readable);
            FD_SET(fstdout, &readable);
            // Wait for output, or a timeout
            if (pselect(fstdout + 1, &readable, NULL, NULL, &waitperiod, NULL) == -1) {
                // Retry if interrupted by a signal
                if ((errno == EINTR) && (trialNumber < 3)) {
                    trialNumber++;
                    continue;
                }
                ppl_error(&c->errcontext, ERR_INTERNAL, -1, -1,
                          "Failure of the pselect() function whilst waiting for sed to return data.");
                sigprocmask(SIG_UNBLOCK, &sigs, NULL);
                free(re);
                re = NULL;
                return;
            }
            break;
        }
        // Give up if sed did not respond
        if (!FD_ISSET(fstdout, &readable)) {
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, "Got bored waiting for sed to return data.");
            sigprocmask(SIG_UNBLOCK, &sigs, NULL);
            free(re);
            re = NULL;
            return;
        }

        // Read data back from sed process, until it closes the pipe. The output may be longer than the regular
        // expression buffer, and may arrive in several pieces, so grow the buffer as needed.
        int i = 0;
        // Read all of sed's output, growing the buffer as needed
        while (1) {
            // Double the size of the buffer when it is full
            if (i >= rel - 1) {
                char *grown = (char *) realloc(re, rel * 2);
                // Failure to allocate memory is reported as an error
                if (grown == NULL) {
                    ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
                    close(fstdout);
                    sigprocmask(SIG_UNBLOCK, &sigs, NULL);
                    free(re);
                    re = NULL;
                    return;
                }
                re = grown;
                rel *= 2;
            }
            const ssize_t n = read(fstdout, re + i, rel - 1 - i);
            if ((n < 0) && (errno == EINTR)) continue;
            // Report a failure to read from the pipe
            if (n < 0) {
                ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, "Could not read from pipe to sed.");
                close(fstdout);
                sigprocmask(SIG_UNBLOCK, &sigs, NULL);
                free(re);
                re = NULL;
                return;
            }
            if (n == 0) break; // End of output
            i += (int) n;
        }
        re[i] = '\0';
        close(fstdout);
        sigprocmask(SIG_UNBLOCK, &sigs, NULL);

        // Copy string into string variable
        outstr = (char *) malloc(i + 1);
        // Failure to allocate memory is reported as an error
        if (outstr == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            free(re);
            re = NULL;
            return;
        }
        strcpy(outstr, re);
        free(re);
        re = NULL;
        // Replace the variable's old string with the output from sed
        if (obj->auxilMalloced) {
            free(obj->auxil);
            obj->auxil = NULL;
        }
        obj->auxil = outstr;
        obj->auxilLen = i + 1;
        obj->auxilMalloced = 1;
    }
    return;
}
