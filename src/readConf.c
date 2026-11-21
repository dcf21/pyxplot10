// readConf.c
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

// Reads Pyxplot's configuration files, ~/.pyxplotrc and ./.pyxplotrc, which can change the default values of settings.
// The files are divided into sections: [settings] for the defaults of the set command, [terminal] for the terminal
// session, [colors] for the palette, [latex] for the LaTeX preamble, [variables] and [functions] for definitions to
// make at startup, [units] for new physical units, [filters] for input filters, [script] for any commands, and
// [styling] for the dimensions of postscript output. After reading them, the defaults are copied into the current
// settings.

#define _PPL_READCONF_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include <gsl/gsl_math.h>

#include "commands/set.h"
#include "coreUtils/errorReport.h"
#include "epsMaker/eps_settings.h"
#include "expressions/expCompile.h"
#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "mathsTools/dcfmath.h"
#include "parser/parser.h"
#include "settings/arrows_fns.h"
#include "settings/axes_fns.h"
#include "settings/epsColors.h"
#include "settings/colors.h"
#include "settings/labels_fns.h"
#include "settings/papersizes.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "settings/withWords_fns.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "readConf.h"

//! _ReadConfig_FetchKey - Read a line of a configuration file, in the format <key=value>, and extract the key
//! \param [in] line - The line of the configuration file
//! \param [out] out - Character buffer into which to write the stripped key

static void _ReadConfig_FetchKey(const char *line, char *out) {
    char *scan = out;
    while ((*line != '\0') && ((*(scan) = *(line++)) != '=')) scan++;
    *scan = '\0';
    ppl_strStrip(out, out);
    return;
}

//! _ReadConfig_FetchValue - Read a line of a configuration file, in the format <key=value>, and extract the value
//! \param [in] line - The line of the configuration file
//! \param [out] out - Character buffer into which to write the stripped value

static void _ReadConfig_FetchValue(const char *line, char *out) {
    char *scan = out;
    while ((*line != '\0') && (*(line++) != '='));
    while (*line != '\0') *(scan++) = *(line++);
    *scan = '\0';
    ppl_strStrip(out, out);
    return;
}

//! readConfigCrangeFlag - Read the value of a boolean configuration setting for one of the colour ranges c1-c4
//! \param [in] c - The Pyxplot context
//! \param [in] setvalue - The value of the setting
//! \param [out] out - The setting to set
//! \return Zero on success, or -1 if the value was illegal

static int readConfigCrangeFlag(const ppl_context *c, const char *setvalue, int *out) {
    const int i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_BOOL_INT, SW_BOOL_STR);
    if (i <= 0) return -1;
    *out = i;
    return 0;
}

//! readConfigCrangeLimit - Read the value of the upper or lower limit of one of the colour ranges c1-c4
//! \param [in] setvalue - The value of the setting: either "auto" or a number
//! \param [out] limit - The limit of the range
//! \param [out] limitAuto - The setting of whether the limit is chosen automatically
//! \return Zero on success, or -1 if the value was illegal

static int readConfigCrangeLimit(const char *setvalue, pplObj *limit, int *limitAuto) {
    int nChars;
    // The limit may be auto, or a finite number
    if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
        *limitAuto = SW_BOOL_TRUE;
        return 0;
    }
    const double fl = ppl_getFloat(setvalue, &nChars);
    if ((!gsl_finite(fl)) || (nChars != strlen(setvalue))) return -1;
    limit->real = fl;
    *limitAuto = SW_BOOL_FALSE;
    return 0;
}

//! readConfigCrange - Read a configuration setting for one of the colour ranges c1-c4, e.g. C1RANGE_LOG
//! \param [in] c - The Pyxplot context
//! \param [in] setkey - The name of the setting, in upper case
//! \param [in] setvalue - The value of the setting
//! \param [in] ConfigFname - The filename of the configuration file, used in error messages
//! \param [in] linecounter - The line number in the configuration file, used in error messages
//! \return Zero if <setkey> is not a colour range setting, 1 if it is and was read successfully, or -1 if it is but
//! its value was illegal (in which case a warning has been issued)

static int readConfigCrange(ppl_context *c, const char *setkey, const char *setvalue, const char *ConfigFname,
                            const int linecounter) {
    static const char *const suffixes[] = {"LOG", "MIN", "MIN_AUTO", "MAX", "MAX_AUTO", "RENORM", "REVERSE"};
    static const char *const names[] = {"log", "min", "min_auto", "max", "max_auto", "renorm", "reverse"};
    const int nSuffixes = sizeof(suffixes) / sizeof(suffixes[0]);
    pplset_graph *g = &c->set->graph_default;
    char key[SSTR_LENGTH], msg[PPL_MSG_LENGTH];
    int status = 0;

    // Find which of the seven settings of which of the four color ranges the key names
    for (int C = 0; C < 4; C++)
        // Each setting
        for (int j = 0; j < nSuffixes; j++) {
            ppl_snprintfTrunc(key, sizeof(key), "C%dRANGE_%s", C + 1, suffixes[j]);
            if (strcmp(setkey, key) != 0) continue;

            // Read the value of the setting
            switch (j) {
                case 0:
                    status = readConfigCrangeFlag(c, setvalue, &g->Clog[C]);
                    break;
                case 1:
                    status = readConfigCrangeLimit(setvalue, &g->Cmin[C], &g->Cminauto[C]);
                    break;
                case 2:
                    status = readConfigCrangeFlag(c, setvalue, &g->Cminauto[C]);
                    break;
                case 3:
                    status = readConfigCrangeLimit(setvalue, &g->Cmax[C], &g->Cmaxauto[C]);
                    break;
                case 4:
                    status = readConfigCrangeFlag(c, setvalue, &g->Cmaxauto[C]);
                    break;
                case 5:
                    status = readConfigCrangeFlag(c, setvalue, &g->Crenorm[C]);
                    break;
                default:
                    status = readConfigCrangeFlag(c, setvalue, &g->Creverse[C]);
                    break;
            }

            // Warn about an invalid value
            if (status == 0) return 1;
            ppl_snprintfTrunc(msg, sizeof(msg),
                              "Error in line %d of configuration file %s: Illegal value for setting <c%dRange_%s>.",
                              linecounter, ConfigFname, C + 1, names[j]);
            ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
            return -1;
        }
    return 0;
}

//! readConfigUnitName - Read one of the names of a unit, from the definition of a unit in the [units] section of a
//! configuration file, e.g. "m / metre / m : length"
//! \param [in] c - The Pyxplot context
//! \param [in] setkey - The definition of the unit
//! \param [in,out] pos - The position in <setkey> where the name starts; advanced to the start of the next name
//! \param [out] output - Set to a newly malloced copy of the name
//! \param [in] fallback - The name to copy if this name is blank, or NULL if this name may not be blank
//! \param [in] type - The type of name, used in error messages, e.g. "unit"
//! \param [in] sep - The character which separates this name from the next
//! \param [in] LaTeX - If true, the name may contain any characters except <sep>; otherwise it must be a valid
//! variable name
//! \param [in] ConfigFname - The filename of the configuration file, used in error messages
//! \param [in] linecounter - The line number in the configuration file, used in error messages
//! \return Zero on success, or nonzero on failure (in which case a warning has been issued)

static int readConfigUnitName(ppl_context *c, const char *setkey, int *pos, char **output, const char *fallback,
                              const char *type, const char sep, const int LaTeX, const char *ConfigFname,
                              const int linecounter) {
    char msg[PPL_MSG_LENGTH];
    const int start = *pos;
    int i = start;

    // Find the end of the name
    if (!LaTeX) {
        if (isalpha(setkey[i])) do {
                i++;
            } while ((isalnum(setkey[i])) || (setkey[i] == '_'));
    } else {
        while ((setkey[i] != sep) && (setkey[i] != '\0')) i++;
    }

    // Store a copy of the name, or of the fallback if the name is blank
    if ((i == start) && (fallback == NULL)) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Error in line %d of configuration file %s: Illegal %s name.", linecounter,
                          ConfigFname, type);
        ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
        return 1;
    }
    // Use the fallback, or a copy of the name
    if (i == start) {
        *output = strdup(fallback);
    } else {
        *output = (char *) malloc(i - start + 1);
        if (*output != NULL) {
            memcpy(*output, setkey + start, i - start);
            (*output)[i - start] = '\0';
        }
    }
    // Report a failure to allocate memory
    if (*output == NULL) {
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Error in line %d of configuration file %s: Out of memory error whilst generating new unit.",
                          linecounter, ConfigFname);
        ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
        return 1;
    }

    // Skip over the separator, and any whitespace around it
    while (((unsigned char) setkey[i] <= ' ') && (setkey[i] != '\0')) i++;
    if (setkey[i] == sep) i++;
    while (((unsigned char) setkey[i] <= ' ') && (setkey[i] != '\0')) i++;
    *pos = i;
    return 0;
}

//! readConf_execLine - Execute a line of a configuration file as a Pyxplot command, ignoring blank lines
//! \param [in] c - The Pyxplot context
//! \param [in] linebuffer - The line of the configuration file
//! \param [in] linecounter - The line number within the configuration file, used in error messages
//! \param [in] ConfigFname - The filename of the configuration file, used in error messages
//! \param [in] command - The parser's name for the command which the line must be, e.g. "var_set"

static void readConf_execLine(ppl_context *c, const char *linebuffer, const int linecounter, const char *ConfigFname,
                              const char *command) {
    parserLine *pl = NULL;
    parserStatus *ps = NULL;
    int i;
    // If line is blank, ignore it
    for (i = 0; (linebuffer[i] != '\0') && ((unsigned char) linebuffer[i] <= ' '); i++);
    if (linebuffer[i] == '\0') return;

    ppl_error_setstreaminfo(&c->errcontext, linecounter, ConfigFname);
    ppl_parserStatInit(&ps, &pl);
    // Failure to allocate memory is reported as an error
    if ((ps == NULL) || (c->inputLineBuffer == NULL)) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
        return;
    }
    ppl_tbClear(c);

    // Compile and execute the line
    const int stat = ppl_parserCompile(c, ps, c->errcontext.error_input_linenumber, c->errcontext.error_input_sourceId,
                                       c->errcontext.error_input_filename, (char *) linebuffer, 0, 0);
    if ((!stat) && (!c->errStat.status) && (ps->blockDepth == 0)) ppl_parserExecute(c, *ps->rootpl, command, 0, 1);
    if (stat || c->errStat.status) ppl_tbWrite(c);
    ppl_parserStatFree(&ps);
}

//! ppl_readConfigFile - Read a Pyxplot configuration file, applying the settings in each of its sections
//! \param [in] c - The Pyxplot context
//! \param [in] ConfigFname - The filename of the configuration file

static void ppl_readConfigFile(ppl_context *c, const char *ConfigFname) {
    // Variables for reading the file
    char msg[PPL_MSG_LENGTH];
    char linebuffer[LSTR_LENGTH], setkey[LSTR_LENGTH], setvalue[LSTR_LENGTH], ColorName[SSTR_LENGTH];
    char *linebufferPtr = &linebuffer[0];
    char errtext[LSTR_LENGTH];
    FILE *infile;
    int state = -1;
    int linecounter = 0;
    int i, j, k, crangeStatus;
    double fl, PaperHeight, PaperWidth;

    // Log progress in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Scanning configuration file %s.", ConfigFname);
        ppl_log(&c->errcontext, msg);
    }

    // A missing configuration file is not an error
    if ((infile = fopen(ConfigFname, "r")) == NULL) {
        if (DEBUG) { ppl_log(&c->errcontext, "File does not exist."); }
        return;
    }

    // Read each line of the file, noting which section it is in
    while ((!feof(infile)) && (!ferror(infile))) {
        linecounter++;
        ppl_file_readline(infile, &linebufferPtr, NULL, LSTR_LENGTH);
        ppl_strStrip(linebuffer, linebuffer);
        if (linebuffer[0] == '\0') continue;
        // Lines in square brackets start new sections
        else if (ppl_strCmpNoCase(linebuffer, "[settings]") == 0) {
            state = 1;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[terminal]") == 0) {
            state = 2;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[colours]") == 0) {
            state = 3;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[colors]") == 0) {
            state = 3;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[latex]") == 0) {
            state = 4;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[variables]") == 0) {
            state = 5;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[functions]") == 0) {
            state = 6;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[units]") == 0) {
            state = 7;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[filters]") == 0) {
            state = 8;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[script]") == 0) {
            state = 9;
            continue;
        } else if (ppl_strCmpNoCase(linebuffer, "[styling]") == 0) {
            state = 10;
            continue;
        }

        // Split the line into its key and value
        _ReadConfig_FetchKey(linebuffer, setkey);
        _ReadConfig_FetchValue(linebuffer, setvalue);

        if (state == 1) // [settings] section
        {
            ppl_strUpper(setkey, setkey);
            // Each setting in the [settings] section sets a default value
            if (strcmp(setkey, "ASPECT") == 0) {
                if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
                    c->set->graph_default.AutoAspect = SW_ONOFF_ON;
                } else if (fl = ppl_getFloat(setvalue, &i),
                           ((gsl_finite(fl)) && (i == strlen(setvalue)) && (fl >= 1e-6) && (fl <= 1e4))) {
                    c->set->graph_default.aspect = fl;
                    c->set->graph_default.AutoAspect = SW_ONOFF_OFF;
                } else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <aspect>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "AUTOASPECT") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->graph_default.AutoAspect = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <autoAspect>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "ZASPECT") == 0) {
                if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
                    c->set->graph_default.AutoZAspect = SW_ONOFF_ON;
                } else if (fl = ppl_getFloat(setvalue, &i),
                           ((gsl_finite(fl)) && (i == strlen(setvalue)) && (fl >= 1e-6) && (fl <= 1e4))) {
                    c->set->graph_default.zaspect = fl;
                    c->set->graph_default.AutoZAspect = SW_ONOFF_OFF;
                } else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <zAspect>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "AUTOZASPECT") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->graph_default.AutoZAspect = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <autoZAspect>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "AXESCOLOUR") == 0) || (strcmp(setkey, "AXESCOLOR") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_COLOR_INT, SW_COLOR_STR)) > 0)
                    c->set->graph_default.AxesColor = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <color>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "AXISUNITSTYLE") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_AXISUNITSTY_INT, SW_AXISUNITSTY_STR)) > 0)
                    c->set->graph_default.AxisUnitStyle = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <axisUnitStyle>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "BACKUP") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.backup = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <backup>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "BAR") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.bar = fl;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <bar>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "BINORIGIN") == 0) {
                // The value may be auto, or a number
                if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
                    c->set->term_default.BinOriginAuto = 1;
                } else if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->term_default.BinOrigin.real = fl;
                    c->set->term_default.BinOriginAuto = 0;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <binOrigin>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "BINWIDTH") == 0) {
                // The value may be auto, or a number
                if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
                    c->set->term_default.BinWidthAuto = 1;
                } else if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->term_default.BinWidth.real = fl;
                    c->set->term_default.BinWidthAuto = (fl < 0.0);
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <binWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "BOXFROM") == 0) {
                // The value may be auto, or a number
                if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
                    c->set->graph_default.BoxFromAuto = 1;
                } else if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.BoxFrom.real = fl;
                    c->set->graph_default.BoxFromAuto = 0;
                } else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <boxFrom>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "BOXWIDTH") == 0) {
                // The value may be auto, or a number
                if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
                    c->set->graph_default.BoxWidthAuto = 1;
                } else if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.BoxWidth.real = fl;
                    c->set->graph_default.BoxWidthAuto = (fl < 0.0);
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <boxWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "CALENDARIN") == 0) {
                // The calendar is named, with muslim accepted as an alternative name for the Islamic calendar
                if (ppl_strCmpNoCase(setvalue, "muslim") == 0) {
                    c->set->term_default.CalendarIn = SW_CALENDAR_ISLAMIC;
                } else if (ppl_strCmpNoCase(setvalue, "jewish") == 0) {
                    c->set->term_default.CalendarIn = SW_CALENDAR_HEBREW;
                } else if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_CALENDAR_INT, SW_CALENDAR_STR)) > 0)
                    c->set->term_default.CalendarIn = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <calendarIn>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "CALENDAROUT") == 0) {
                // The calendar is named, with muslim accepted as an alternative name for the Islamic calendar
                if (ppl_strCmpNoCase(setvalue, "muslim") == 0) {
                    c->set->term_default.CalendarOut = SW_CALENDAR_ISLAMIC;
                } else if (ppl_strCmpNoCase(setvalue, "jewish") == 0) {
                    c->set->term_default.CalendarOut = SW_CALENDAR_HEBREW;
                } else if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_CALENDAR_INT, SW_CALENDAR_STR)) > 0)
                    c->set->term_default.CalendarOut = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <calendarOut>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "CLIP") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->graph_default.clip = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <clip>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "COLKEY") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->graph_default.ColKey = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <colKey>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "COLKEYPOS") == 0) {
                // The position is named, with outside accepted as an alternative to right
                if (ppl_strCmpNoCase(setvalue, "outside") == 0) {
                    c->set->graph_default.ColKeyPos = SW_COLKEYPOS_R;
                } else if (ppl_strCmpNoCase(setvalue, "below") == 0) {
                    c->set->graph_default.ColKeyPos = SW_COLKEYPOS_B;
                } else if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_COLKEYPOS_INT, SW_COLKEYPOS_STR)) >
                           0)
                    c->set->graph_default.ColKeyPos = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <colKeyPos>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "COLOUR") == 0) || (strcmp(setkey, "COLOR") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.color = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <color>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "CONTOURS") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.ContoursN = ppl_max((int) fl, 2);
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <contours>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "CONTOURS_LABEL") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->graph_default.ContoursLabel = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <contours_label>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            }

            // Settings for the color ranges, e.g. C1RANGE_LOG
            else if ((crangeStatus = readConfigCrange(c, setkey, setvalue, ConfigFname, linecounter)) != 0) {
                // A setting for the colour range c1-c4; skip to the next line if its value was illegal
                if (crangeStatus < 0) continue;
            } else if (strcmp(setkey, "DATASTYLE") == 0) {
                // The value must be one of the recognised names
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_STYLE_INT, SW_STYLE_STR)) > 0) {
                    c->set->graph_default.dataStyle.linespoints = i;
                    c->set->graph_default.dataStyle.USElinespoints = 1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <dataStyle>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "DISPLAY") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.display = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <display>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "DPI") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)) && (fl > 2)))
                    c->set->term_default.dpi = fl;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <dpi>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "FONTSIZE") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.FontSize = fl;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <fontSize>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "FUNCSTYLE") == 0) {
                // The value must be one of the recognised names
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_STYLE_INT, SW_STYLE_STR)) > 0) {
                    c->set->graph_default.funcStyle.linespoints = i;
                    c->set->graph_default.funcStyle.USElinespoints = 1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <funcStyle>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "GRID") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->graph_default.grid = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <grid>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "GRIDAXISX") == 0) {
                if (fl = ppl_getFloat(setvalue, &i),
                    ((gsl_finite(fl)) && (i == strlen(setvalue)) && (fl >= 0) && (fl <= MAX_AXES))) {
                    c->set->graph_default.GridAxisX[1] = 0;
                    c->set->graph_default.GridAxisX[(int) fl] = 1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <gridAxisX>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "GRIDAXISY") == 0) {
                if (fl = ppl_getFloat(setvalue, &i),
                    ((gsl_finite(fl)) && (i == strlen(setvalue)) && (fl >= 0) && (fl <= MAX_AXES))) {
                    c->set->graph_default.GridAxisY[1] = 0;
                    c->set->graph_default.GridAxisY[(int) fl] = 1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <gridAxisY>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "GRIDAXISZ") == 0) {
                if (fl = ppl_getFloat(setvalue, &i),
                    ((gsl_finite(fl)) && (i == strlen(setvalue)) && (fl >= 0) && (fl <= MAX_AXES))) {
                    c->set->graph_default.GridAxisZ[1] = 0;
                    c->set->graph_default.GridAxisZ[(int) fl] = 1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <gridAxisZ>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "GRIDMAJCOLOUR") == 0) || (strcmp(setkey, "GRIDMAJCOLOR") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_COLOR_INT, SW_COLOR_STR)) > 0)
                    c->set->graph_default.GridMajColor = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <gridMajColor>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "GRIDMINCOLOUR") == 0) || (strcmp(setkey, "GRIDMINCOLOR") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_COLOR_INT, SW_COLOR_STR)) > 0)
                    c->set->graph_default.GridMinColor = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <gridMinColor>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "KEY") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->graph_default.key = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <key>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "KEYCOLUMNS") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.KeyColumns = ppl_max((int) fl, 0);
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <keyColumns>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "KEYPOS") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_KEYPOS_INT, SW_KEYPOS_STR)) > 0)
                    c->set->graph_default.KeyPos = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <keyPos>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "KEY_XOFF") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.KeyXOff.real = fl / 100;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <key_XOff>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "KEY_YOFF") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.KeyYOff.real = fl / 100;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <key_YOff>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "LANDSCAPE") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.landscape = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <landscape>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "LINEWIDTH") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.LineWidth = fabs(fl);
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <lineWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "MULTIPLOT") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.multiplot = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <multiPlot>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "NUMCOMPLEX") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.ComplexNumbers = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <numComplex>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "NUMDISPLAY") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_DISPLAY_INT, SW_DISPLAY_STR)) > 0)
                    c->set->term_default.NumDisplay = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <numDisplay>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "NUMERR") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.ExplicitErrors = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <numErr>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "NUMSF") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->term_default.SignificantFigures = ppl_min(ppl_max((int) fl, 1), 30);
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <numSF>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "ORIGINX") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.OriginX.real = fl / 100;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <originX>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "ORIGINY") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.OriginY.real = fl / 100;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <originY>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "OUTPUT") == 0) {
                ppl_snprintfTrunc(c->set->term_default.output, sizeof(c->set->term_default.output), "%s", setvalue);
            } else if (strcmp(setkey, "PAPERHEIGHT") == 0) {
                // Paper sizes may be given as a height and width in millimetres, or by name
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->term_default.PaperHeight.real = fl / 1000;
                    PaperHeight = c->set->term_default.PaperHeight.real * 1000;
                    PaperWidth = c->set->term_default.PaperWidth.real * 1000;
                    ppl_GetPaperName(c->set->term_default.PaperName, &PaperHeight, &PaperWidth);
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <paperHeight>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "PAPERNAME") == 0) {
                {
                    ppl_PaperSizeByName(setvalue, &PaperHeight, &PaperWidth);
                    // Warn about a paper size which is not recognised
                    if (PaperHeight <= 0) {
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "Error in line %d of configuration file %s: Unrecognised papersize specified for setting <paperName>.",
                                linecounter, ConfigFname);
                        ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                        continue;
                    }
                    c->set->term_default.PaperHeight.real = PaperHeight / 1000;
                    c->set->term_default.PaperWidth.real = PaperWidth / 1000;
                    ppl_GetPaperName(c->set->term_default.PaperName, &PaperHeight, &PaperWidth);
                }
            } else if (strcmp(setkey, "PAPERWIDTH") == 0) {
                // The width of the paper, in millimetres
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->term_default.PaperWidth.real = fl / 1000;
                    PaperHeight = c->set->term_default.PaperHeight.real * 1000;
                    PaperWidth = c->set->term_default.PaperWidth.real * 1000;
                    ppl_GetPaperName(c->set->term_default.PaperName, &PaperHeight, &PaperWidth);
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <paperWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "POINTLINEWIDTH") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.PointLineWidth = fl;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <pointLineWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "POINTSIZE") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.PointSize = fl;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <pointSize>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            }
            //      else if (strcmp(setkey, "PROJECTION"   )==0)
            //        if ((i=ppl_fetchSettingByName(&c->errcontext,setvalue,SW_PROJ_INT, SW_PROJ_STR ))>0)                        c->set->graph_default.projection    = i;
            //        else {ppl_snprintfTrunc(msg, sizeof(msg), "Error in line %d of configuration file %s: Illegal value for setting <projection>."   , linecounter, ConfigFname); ppl_warning(&c->errcontext, ERR_PREFORMED, msg); continue; }
            else if (strcmp(setkey, "SAMPLES") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.samples = ppl_max((int) fl, 2);
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <samples>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "SAMPLES_METHOD") == 0)
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_SAMPLEMETHOD_INT, SW_SAMPLEMETHOD_STR)) >
                    0)
                    c->set->graph_default.Sample2DMethod = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <samples_method>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            // The number of samples in each direction for two-dimensional plots, or auto
            else if (strcmp(setkey, "SAMPLES_X") == 0) {
                // The value may be auto, or a number
                if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
                    c->set->graph_default.SamplesXAuto = SW_BOOL_TRUE;
                } else if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.SamplesX = ppl_max((int) fl, 2);
                    c->set->graph_default.SamplesXAuto = SW_BOOL_FALSE;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <samples_x>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "SAMPLES_X_AUTO") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_BOOL_INT, SW_BOOL_STR)) > 0)
                    c->set->graph_default.SamplesXAuto = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <samples_x_auto>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "SAMPLES_Y") == 0) {
                // The value may be auto, or a number
                if (ppl_strCmpNoCase(setvalue, "auto") == 0) {
                    c->set->graph_default.SamplesYAuto = SW_BOOL_TRUE;
                } else if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.SamplesY = ppl_max((int) fl, 2);
                    c->set->graph_default.SamplesYAuto = SW_BOOL_FALSE;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <samples_y>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "SAMPLES_Y_AUTO") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_BOOL_INT, SW_BOOL_STR)) > 0)
                    c->set->graph_default.SamplesYAuto = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <samples_y_auto>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TERMANTIALIAS") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.TermAntiAlias = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <termAntiAlias>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TERMENLARGE") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.TermEnlarge = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <termEnlarge>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "ENLARGE") ==
                       0) // ENLARGE, as opposed to TERMENLARGE is supported for back-compatibility with Pyxplot 0.7
            {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.TermEnlarge = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <enlarge>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TERMINVERT") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.TermInvert = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <termInvert>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TERMTRANSPARENT") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.TermTransparent = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <termTransparent>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TERMTYPE") == 0) {
                // The value must be one of the recognised names
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_TERMTYPE_INT, SW_TERMTYPE_STR)) > 0) {
                    c->set->term_default.TermType = i;
                    c->termtypeSetInConfigfile = 1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <termType>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "TEXTCOLOUR") == 0) || (strcmp(setkey, "TEXTCOLOR") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_COLOR_INT, SW_COLOR_STR)) > 0)
                    c->set->graph_default.TextColor = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <textColor>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TEXTHALIGN") == 0) {
                // The alignment is named, with centre accepted as an alternative spelling
                if (ppl_strCmpNoCase(setvalue, "centre") == 0) {
                    c->set->graph_default.TextHAlign = SW_HALIGN_CENT;
                } else if (ppl_strCmpNoCase(setvalue, "middle") == 0) {
                    c->set->graph_default.TextHAlign = SW_HALIGN_CENT;
                } else if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_HALIGN_INT, SW_HALIGN_STR)) > 0)
                    c->set->graph_default.TextHAlign = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <textHAlign>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TEXTVALIGN") == 0) {
                // The alignment is named, with centre accepted as an alternative spelling
                if (ppl_strCmpNoCase(setvalue, "centre") == 0) {
                    c->set->graph_default.TextVAlign = SW_VALIGN_CENT;
                } else if (ppl_strCmpNoCase(setvalue, "middle") == 0) {
                    c->set->graph_default.TextVAlign = SW_VALIGN_CENT;
                } else if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_VALIGN_INT, SW_VALIGN_STR)) > 0)
                    c->set->graph_default.TextVAlign = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <textVAlign>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TITLE") == 0) {
                ppl_snprintfTrunc(c->set->graph_default.title, sizeof(c->set->graph_default.title), "%s", setvalue);
            } else if (strcmp(setkey, "TITLE_XOFF") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.TitleXOff.real = fl / 100;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <title_xOff>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TITLE_YOFF") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.TitleYOff.real = fl / 100;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <title_yOff>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TRANGE_LOG") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_BOOL_INT, SW_BOOL_STR)) > 0)
                    c->set->graph_default.Tlog = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <tRange_log>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TRANGE_MIN") == 0) {
                // The lower limit of the parametric variable t
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.Tmin.real = fl;
                    c->set->graph_default.USE_T_or_uv = 1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <tRange_min>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "TRANGE_MAX") == 0) {
                // The upper limit of the parametric variable t
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.Tmax.real = fl;
                    c->set->graph_default.USE_T_or_uv = 1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <tRange_max>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "UNITABBREV") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.UnitDisplayAbbrev = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <unitAbbrev>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "UNITANGLEDIMLESS") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.UnitAngleDimless = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <unitAngleDimless>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "UNITPREFIX") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->set->term_default.UnitDisplayPrefix = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <unitPrefix>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "UNITSCHEME") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_UNITSCH_INT, SW_UNITSCH_STR)) > 0)
                    c->set->term_default.UnitScheme = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <unitScheme>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "URANGE_LOG") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_BOOL_INT, SW_BOOL_STR)) > 0)
                    c->set->graph_default.Ulog = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <uRange_log>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "URANGE_MIN") == 0) {
                // The lower limit of the parametric variable u
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.Umin.real = fl;
                    c->set->graph_default.USE_T_or_uv = 0;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <uRange_min>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "URANGE_MAX") == 0) {
                // The upper limit of the parametric variable u
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.Umax.real = fl;
                    c->set->graph_default.USE_T_or_uv = 0;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <uRange_max>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "VRANGE_LOG") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_BOOL_INT, SW_BOOL_STR)) > 0)
                    c->set->graph_default.Vlog = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <vRange_log>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "VRANGE_MIN") == 0) {
                // The lower limit of the parametric variable v
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.Vmin.real = fl;
                    c->set->graph_default.USE_T_or_uv = 0;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <vRange_min>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "VRANGE_MAX") == 0) {
                // The upper limit of the parametric variable v
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue)))) {
                    c->set->graph_default.Vmax.real = fl;
                    c->set->graph_default.USE_T_or_uv = 0;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <vRange_max>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "WIDTH") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.width.real = fl / 100;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <width>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "VIEW_XY") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.XYview.real = fl / 180 * M_PI;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <view_xy>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "VIEW_YZ") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    c->set->graph_default.YZview.real = fl / 180 * M_PI;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <view_yz>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else {
                ppl_snprintfTrunc(msg, sizeof(msg),
                                  "Error in line %d of configuration file %s: Unrecognised setting name '%s'.",
                                  linecounter, ConfigFname, setkey);
                ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                continue;
            }
        } else if (state == 2) // [terminal] section
        {
            ppl_strUpper(setkey, setkey);
            // Each setting in the [terminal] section sets a property of the terminal session
            if ((strcmp(setkey, "COLOUR") == 0) || (strcmp(setkey, "COLOR") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->errcontext.session_default.color = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <color>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "COLOUR_ERR") == 0) || (strcmp(setkey, "COLOR_ERR") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_TERMCOL_INT, SW_TERMCOL_STR)) > 0)
                    c->errcontext.session_default.color_err = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <color_err>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "COLOUR_REP") == 0) || (strcmp(setkey, "COLOR_REP") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_TERMCOL_INT, SW_TERMCOL_STR)) > 0)
                    c->errcontext.session_default.color_rep = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <color_rep>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "COLOUR_WRN") == 0) || (strcmp(setkey, "COLOR_WRN") == 0)) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_TERMCOL_INT, SW_TERMCOL_STR)) > 0)
                    c->errcontext.session_default.color_wrn = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <color_wrn>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "SPLASH") == 0) {
                if ((i = ppl_fetchSettingByName(&c->errcontext, setvalue, SW_ONOFF_INT, SW_ONOFF_STR)) > 0)
                    c->errcontext.session_default.splash = i;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s: Illegal value for setting <splash>.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else {
                ppl_snprintfTrunc(msg, sizeof(msg),
                                  "Error in line %d of configuration file %s: Unrecognised setting name '%s'.",
                                  linecounter, ConfigFname, setkey);
                ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                continue;
            }
        } else if (state == 3) // [colors] section
        {
            ppl_strUpper(setkey, setkey);
            // The palette is a comma-separated list of named colors
            if (strcmp(setkey, "PALETTE") == 0) {
                int PalettePos = 0;
                char *StringScan = setvalue;
                // Read each color of the list
                while (strlen(ppl_strCommaSeparatedListScan(&StringScan, ColorName)) != 0) {
                    // Warn if the palette is too long
                    if (PalettePos == PALETTE_LENGTH - 1) {
                        ppl_snprintfTrunc(msg, sizeof(msg),
                                          "Error in line %d of configuration file %s: Specified palette is too long.",
                                          linecounter, ConfigFname);
                        ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                        c->set->palette_default[PalettePos] = -1;
                        continue;
                    } else {
                        ppl_strUpper(ColorName, ColorName);
                        const int ColorNumber = ppl_fetchSettingByName(&c->errcontext, ColorName, SW_COLOR_INT, SW_COLOR_STR);
                        // Warn about a color which is not recognised
                        if (ColorNumber <= 0) {
                            ppl_snprintfTrunc(msg, sizeof(msg),
                                              "Error in line %d of configuration file %s: Color '%s' not recognised.",
                                              linecounter, ConfigFname, ColorName);
                            ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                        } else {
                            c->set->palette_default[PalettePos] = ColorNumber;
                            c->set->paletteS_default[PalettePos] = 0;
                            c->set->palette1_default[PalettePos] = 0.0;
                            c->set->palette2_default[PalettePos] = 0.0;
                            c->set->palette3_default[PalettePos] = 0.0;
                            c->set->palette4_default[PalettePos++] = 0.0;
                        }
                    }
                }
                // Terminate the palette, if it has any colors
                if (PalettePos > 0) {
                    c->set->palette_default[PalettePos] = -1;
                } else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: The specified palette does not contain any colors.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                }
            } else {
                ppl_snprintfTrunc(msg, sizeof(msg),
                                  "Error in line %d of configuration file %s: Unrecognised setting name '%s'.",
                                  linecounter, ConfigFname, setkey);
                ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                continue;
            }
        } else if (state == 4) // [latex] section
        {
            ppl_strUpper(setkey, setkey);
            if (strcmp(setkey, "PREAMBLE") == 0)
                ppl_snprintfTrunc(c->set->term_default.LatexPreamble, sizeof(c->set->term_default.LatexPreamble), "%s",
                                  setvalue);
            // Otherwise, warn about the invalid value and ignore the line
            else {
                ppl_snprintfTrunc(msg, sizeof(msg),
                                  "Error in line %d of configuration file %s: Unrecognised setting name '%s'.",
                                  linecounter, ConfigFname, setkey);
                ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                continue;
            }
        } else if (state == 5) // [variables] section
        {
            // Execute the line as an assignment, with the parser's var_set command
            readConf_execLine(c, linebuffer, linecounter, ConfigFname, "var_set");
        } else if (state == 6) // [functions] section
        {
            // Execute the line as a function definition, with the parser's func_set command
            readConf_execLine(c, linebuffer, linecounter, ConfigFname, "func_set");
        } else if (state == 7) // [units] section
        {
            i = j = 0;
            // There is a fixed limit on the number of units
            if (c->unit_pos == UNITS_MAX) {
                ppl_snprintfTrunc(msg, sizeof(msg),
                                  "Error in line %d of configuration file %s: Unit definition list full.", linecounter,
                                  ConfigFname);
                ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                continue;
            }

            {
                unit *u = &c->unit_database[c->unit_pos];
                // Read the names of the unit, each of which defaults to a previous name if left blank
                if (readConfigUnitName(c, setkey, &i, &u->nameFs, NULL, "unit", '/', 0, ConfigFname, linecounter) ||
                    readConfigUnitName(c, setkey, &i, &u->nameAs, u->nameFs, "unit", '/', 0, ConfigFname,
                                       linecounter) ||
                    readConfigUnitName(c, setkey, &i, &u->nameLs, u->nameAs, "unit", '/', 1, ConfigFname,
                                       linecounter) ||
                    readConfigUnitName(c, setkey, &i, &u->nameFp, u->nameFs, "unit", '/', 0, ConfigFname,
                                       linecounter) ||
                    readConfigUnitName(c, setkey, &i, &u->nameAp, u->nameAs, "unit", '/', 0, ConfigFname,
                                       linecounter) ||
                    readConfigUnitName(c, setkey, &i, &u->nameLp, u->nameAp, "unit", ':', 1, ConfigFname,
                                       linecounter) ||
                    readConfigUnitName(c, setkey, &i, &u->quantity, NULL, "quantity", ' ', 0, ConfigFname, linecounter))
                    continue;
            }

            // A unit with no definition is a new base unit, measuring a new dimension
            if (setvalue[0] == '\0') {
                // There is a fixed limit on the number of base units
                if (c->baseunit_pos == UNITS_MAX_BASEUNITS) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Error in line %d of configuration file %s:\nBase unit definition list full.",
                                      linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
                c->unit_database[c->unit_pos++].exponent[c->baseunit_pos++] = 1;
            } else {
                pplObj setnumeric;
                setnumeric.refCount = 1;
                j = k = -1;
                ppl_unitsStringEvaluate(c, setvalue, &setnumeric, &k, &j, errtext);
                // Report a definition which cannot be evaluated
                if (j >= 0) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Error in line %d of configuration file %s: %s", linecounter,
                                      ConfigFname, errtext);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
                // The whole definition must be evaluated
                if (setvalue[k] != '\0') {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Unexpected trailing matter in definition",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
                // Units must have real values
                if (setnumeric.flagComplex) {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Multiplier in units definition cannot be complex",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
                for (j = 0; j < UNITS_MAX_BASEUNITS; j++)
                    c->unit_database[c->unit_pos].exponent[j] = setnumeric.exponent[j];
                c->unit_database[c->unit_pos].multiplier = setnumeric.real;
                c->unit_pos++;
            }
        } else if (state == 8) // [filters] section
        {
            pplObj setnumeric;
            char *tmp = (char *) malloc(strlen(setvalue) + 1);
            // Store the filter command for files matching the pattern
            if (tmp != NULL) {
                strcpy(tmp, setvalue);
                pplObjStr(&setnumeric, 1, 1, tmp);
                setnumeric.refCount = 1;
                ppl_dictAppendCpy(c->set->filters, setkey, (void *) &setnumeric, sizeof(pplObj));
            }
        } else if (state == 9) // [script] section
        {
            ppl_restoreDefaultSettings(c); // Copy settings for directive_set()

            // Execute the line as a set command
            readConf_execLine(c, linebuffer, linecounter, ConfigFname, "set");

            c->set->term_default = c->set->term_current; // Copy changed settings into defaults
            c->set->graph_default = c->set->graph_current;
            // Copy each palette color into the defaults
            for (i = 0; i < PALETTE_LENGTH; i++) {
                c->set->palette_default[i] = c->set->palette_current[i];
                c->set->paletteS_default[i] = c->set->paletteS_current[i];
                c->set->palette1_default[i] = c->set->palette1_current[i];
                c->set->palette2_default[i] = c->set->palette2_current[i];
                c->set->palette3_default[i] = c->set->palette3_current[i];
                c->set->palette4_default[i] = c->set->palette4_current[i];
            }
            // Copy each axis into the defaults
            for (i = 0; i < MAX_AXES; i++) {
                pplaxis_destroy(c, &(c->set->XAxesDefault[i]));
                pplaxis_copy(c, &(c->set->XAxesDefault[i]), &(c->set->XAxes[i]));
                pplaxis_destroy(c, &(c->set->YAxesDefault[i]));
                pplaxis_copy(c, &(c->set->YAxesDefault[i]), &(c->set->YAxes[i]));
                pplaxis_destroy(c, &(c->set->ZAxesDefault[i]));
                pplaxis_copy(c, &(c->set->ZAxesDefault[i]), &(c->set->ZAxes[i]));
            }
            // Copy each numbered plot style into the defaults
            for (i = 0; i < MAX_PLOTSTYLES; i++) {
                ppl_withWordsDestroy(c, &(c->set->plot_styles_default[i]));
                ppl_withWordsCpy(c, &(c->set->plot_styles_default[i]), &(c->set->plot_styles[i]));
            }
            pplarrow_list_destroy(c, &c->set->pplarrow_list_default);
            pplarrow_list_copy(c, &c->set->pplarrow_list_default, &c->set->pplarrow_list);
            ppllabel_list_destroy(c, &c->set->ppllabel_list_default);
            ppllabel_list_copy(c, &c->set->ppllabel_list_default, &c->set->ppllabel_list);
        } else if (state == 10) // [styling] section
        {
            ppl_strUpper(setkey, setkey);
            // Each setting in the [styling] section scales one of the dimensions used in postscript output
            if (strcmp(setkey, "BASELINE_LINEWIDTH") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_DEFAULT_LINEWIDTH = fl * EPS_BASE_DEFAULT_LINEWIDTH;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <baseline_lineWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "BASELINE_POINTSIZE") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_DEFAULT_PS = fl * EPS_BASE_DEFAULT_PS;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <baseline_pointSize>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "ARROW_HEADANGLE") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_ARROW_ANGLE = fl * M_PI / 180;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <arrow_headAngle>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "ARROW_HEADSIZE") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_ARROW_HEADSIZE = fl * EPS_BASE_ARROW_HEADSIZE;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <arrow_headSize>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "ARROW_HEADBACKINDENT") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_ARROW_CONSTRICT = fl;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <arrow_headBackIndent>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "AXES_SEPARATION") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_AXES_SEPARATION = fl * EPS_BASE_AXES_SEPARATION;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <axes_separation>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "AXES_TEXTGAP") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_AXES_TEXTGAP = fl * EPS_BASE_AXES_TEXTGAP;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <axes_textGap>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "AXES_LINEWIDTH") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_AXES_LINEWIDTH = fl * EPS_BASE_AXES_LINEWIDTH;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <axes_lineWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "AXES_MAJTICKLEN") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_AXES_MAJTICKLEN = fl * EPS_BASE_AXES_MAJTICKLEN;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <axes_majTickLen>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "AXES_MINTICKLEN") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_AXES_MINTICKLEN = fl * EPS_BASE_AXES_MINTICKLEN;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <axes_minTickLen>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "COLOURSCALE_MARGIN") == 0) || (strcmp(setkey, "COLORSCALE_MARGIN") == 0)) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_COLORSCALE_MARGIN = fl * EPS_BASE_COLORSCALE_MARG;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <colorScale_margin>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if ((strcmp(setkey, "COLOURSCALE_WIDTH") == 0) || (strcmp(setkey, "COLORSCALE_WIDTH") == 0)) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_COLORSCALE_WIDTH = fl * EPS_BASE_COLORSCALE_WIDTH;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <colorScale_width>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "GRID_MAJLINEWIDTH") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_GRID_MAJLINEWIDTH = fl * EPS_BASE_GRID_MAJLINEWIDTH;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <grid_majLineWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else if (strcmp(setkey, "GRID_MINLINEWIDTH") == 0) {
                if (fl = ppl_getFloat(setvalue, &i), ((gsl_finite(fl)) && (i == strlen(setvalue))))
                    EPS_GRID_MINLINEWIDTH = fl * EPS_BASE_GRID_MINLINEWIDTH;
                // Otherwise, warn about the invalid value and ignore the line
                else {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "Error in line %d of configuration file %s: Illegal value for setting <grid_minLineWidth>.",
                            linecounter, ConfigFname);
                    ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                    continue;
                }
            } else {
                ppl_snprintfTrunc(msg, sizeof(msg),
                                  "Error in line %d of configuration file %s: Unrecognised setting name '%s'.",
                                  linecounter, ConfigFname, setkey);
                ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
                continue;
            }
        } else {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Error in line %d of configuration file %s: Settings need to be preceded by a block name such as [settings].",
                    linecounter, ConfigFname);
            ppl_warning(&c->errcontext, ERR_PREFORMED, msg);
            break;
        }
    }
    fclose(infile);
    return;
}

//! ppl_restoreDefaultSettings - Copy the default settings, as modified by configuration files, into the current
//! settings, including the palettes, axes, numbered plot styles, arrows and labels
//! \param [in] c - The Pyxplot context

void ppl_restoreDefaultSettings(ppl_context *c) {
    c->set->term_current = c->set->term_default;
    c->set->graph_current = c->set->graph_default;
    // Copy each palette color
    for (int i = 0; i < PALETTE_LENGTH; i++) {
        c->set->palette_current[i] = c->set->palette_default[i];
        c->set->paletteS_current[i] = c->set->paletteS_default[i];
        c->set->palette1_current[i] = c->set->palette1_default[i];
        c->set->palette2_current[i] = c->set->palette2_default[i];
        c->set->palette3_current[i] = c->set->palette3_default[i];
        c->set->palette4_current[i] = c->set->palette4_default[i];
    }
    // Copy each axis
    for (int i = 0; i < MAX_AXES; i++) {
        pplaxis_destroy(c, &(c->set->XAxes[i]));
        pplaxis_copy(c, &(c->set->XAxes[i]), &(c->set->XAxesDefault[i]));
        pplaxis_destroy(c, &(c->set->YAxes[i]));
        pplaxis_copy(c, &(c->set->YAxes[i]), &(c->set->YAxesDefault[i]));
        pplaxis_destroy(c, &(c->set->ZAxes[i]));
        pplaxis_copy(c, &(c->set->ZAxes[i]), &(c->set->ZAxesDefault[i]));
    }
    // Copy each numbered plot style
    for (int i = 0; i < MAX_PLOTSTYLES; i++) {
        ppl_withWordsDestroy(c, &(c->set->plot_styles[i]));
        ppl_withWordsCpy(c, &(c->set->plot_styles[i]), &(c->set->plot_styles_default[i]));
    }
    // Copy the arrows and labels
    pplarrow_list_destroy(c, &c->set->pplarrow_list);
    pplarrow_list_copy(c, &c->set->pplarrow_list, &c->set->pplarrow_list_default);
    ppllabel_list_destroy(c, &c->set->ppllabel_list);
    ppllabel_list_copy(c, &c->set->ppllabel_list, &c->set->ppllabel_list_default);
}

//! ppl_readconfig - Read the user's configuration files, ~/.pyxplotrc and ./.pyxplotrc, if they exist
//! \param [in] c - The Pyxplot context

void ppl_readconfig(ppl_context *c) {
    // Variables for reading the configuration files
    char ConfigFname[FNAME_LENGTH];

    // Read the configuration file in the user's home directory, then any in the current directory
    if (snprintf(ConfigFname, FNAME_LENGTH, "%s%s%s", c->errcontext.session_default.homedir, PATHLINK, ".pyxplotrc") <
        FNAME_LENGTH)
        ppl_readConfigFile(c, ConfigFname);
    ppl_snprintfTrunc(ConfigFname, sizeof(ConfigFname), "%s", ".pyxplotrc");
    ppl_readConfigFile(c, ConfigFname);

    // Copy Default Settings to Current Settings
    ppl_restoreDefaultSettings(c);

    // Copy List of Preferred Units
    ppl_listFree(c->unit_PreferredUnits_default);
    c->unit_PreferredUnits_default = ppl_listCpy(c->unit_PreferredUnits, 1, sizeof(PreferredUnit));
    return;
}
