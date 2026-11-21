// moduleTime.c
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

// The time module: functions which make date objects from calendar dates, Julian dates and unix times, measure and
// format the intervals between dates, format dates as strings, and pause execution. The conversions themselves are in
// userspace/calendars.c.

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "coreUtils/dict.h"

#include "settings/settings.h"

#include "stringTools/asciidouble.h"

#include "userspace/calendars.h"
#include "userspace/pplObj.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/unitsArithmetic.h"

#include "defaultObjs/moduleTime.h"
#include "defaultObjs/defaultFuncs.h"
#include "defaultObjs/defaultFuncsChecks.h"

//! pplfunc_timefromUnix - Implement the function time.fromUnix(t) in the time module, which creates a date object from
//! the specified unix time
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_timefromUnix(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "fromUnix(t)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    pplObjDate(&OUTPUT, 0, in[0].real);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_timefromJD - Implement the function time.fromJD(t) in the time module, which creates a date object from the
//! specified Julian date
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_timefromJD(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "fromJD(t)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    pplObjDate(&OUTPUT, 0, 86400.0 * (in[0].real - 2440587.5));
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_timefromMJD - Implement the function time.fromMJD(t) in the time module, which creates a date object from
//! the specified modified Julian date
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_timefromMJD(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "fromMJD(t)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    pplObjDate(&OUTPUT, 0, 86400.0 * (in[0].real - 40587.0));
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_timefromCalendar - Implement the function time.fromCalendar(year,month,day,hour,min,sec) in the time module,
//! which creates a date object from the specified calendar date. See also 'set calendar'
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_timefromCalendar(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // Each component of the date must be a real, dimensionless, finite number, and the hour, minute and second must be
    // integers
    char *FunctionDescription = "fromCalendar(year,month,day,hour,min,sec)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    double offset;
    if (ppl_funcCheckRealDimensionlessFinite(&call, &in[0], "year")) return;
    if (ppl_funcCheckRealDimensionlessFinite(&call, &in[1], "month")) return;
    if (ppl_funcCheckRealDimensionlessFinite(&call, &in[2], "day")) return;
    if (ppl_funcCheckRealDimensionlessFinite(&call, &in[3], "hour")) return;
    if (ppl_funcCheckRealDimensionlessFinite(&call, &in[4], "min")) return;
    if (ppl_funcCheckRealDimensionlessFinite(&call, &in[5], "sec")) return;
    if (ppl_funcCheckSignedInt(&call, &in[0], "year", "function's first input (year) must be an integer")) return;
    if (ppl_funcCheckInt(&call, &in[1], "month", "function's second input (month) must be an integer")) return;
    if (ppl_funcCheckInt(&call, &in[2], "day", "function's third input (day) must be an integer")) return;
    if (ppl_funcCheckInt(&call, &in[3], "hour", "function's fourth input (hours) must be an integer")) return;
    if (ppl_funcCheckInt(&call, &in[4], "min", "function's fifth input (minutes) must be an integer")) return;
    if (ppl_funcCheckInt(&call, &in[5], "sec", "function's sixth input (seconds) must be an integer")) return;
    // The optional timezone must be a string
    if ((nArgs > 6) && (in[6].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a string as its seventh argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[6].objType]);
        return;
    }
    if (nArgs > 6) ppl_calendarTimezoneSet(c, 1, (char *) in[6].auxil);
    else ppl_calendarTimezoneSet(c, 0, NULL);
    // The seconds may have a fractional part, which is added after converting the whole seconds
    const double t = ppl_toUnixTime(c, (int) in[0].real, (int) in[1].real, (int) in[2].real, (int) in[3].real,
                              (int) in[4].real, (int) floor(in[5].real), status,
                              errText) + (in[5].real - floor(in[5].real));
    ppl_calendarTimezoneOffset(c, t, NULL, &offset);
    ppl_calendarTimezoneUnset(c);
    pplObjDate(&OUTPUT, 0, t - offset);
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_timeInterval - Implement the function time.interval(t2,t1) in the time module, which returns the numerical
//! time interval between date objects t1 and t2
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_timeInterval(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "interval(t2,t1)";
    // Both arguments must be dates
    if (in[0].objType != PPLOBJ_DATE) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a date as its first argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    // The second argument must be a date
    if (in[1].objType != PPLOBJ_DATE) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a date as its second argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    pplObjNum(&OUTPUT, 0, in[0].real - in[1].real, 0);
    ppl_funcSetOutputUnit(c, &OUTPUT, UNIT_TIME);
}

//! pplfunc_timeIntervalStr - Implement the function time.intervalStr(t2,t1,<s>) in the time module, which returns a
//! string representation of the time interval between date objects t1 and t2; a format string may optionally be
//! supplied
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_timeIntervalStr(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // Both arguments must be dates, and the optional format a string
    char *FunctionDescription = "intervalStr(t2,t1,<s>)";
    char *format = NULL, *out = NULL;
    // The first argument must be a date
    if (in[0].objType != PPLOBJ_DATE) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a date as its first argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    // The second argument must be a date
    if (in[1].objType != PPLOBJ_DATE) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a date as its second argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[1].objType]);
        return;
    }
    // The optional format must be a string
    if ((nArgs > 2) && (in[2].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a string as its third argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[2].objType]);
        return;
    }

    // A buffer for the output string
    out = (char *) malloc(8192);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }

    if (nArgs > 2) // Format specified
    {
        format = (char *) in[2].auxil;
        ppl_timeDiffStr(c, out, 8192, in[1].real, in[0].real, format, status, errText); // The interval from t1 to t2
        // Report a bad format string
        if (*status) {
            *errType = ERR_NUMERICAL;
            return;
        }
    } else // Format not specified
    {
        int i = 0;
        const int s = (int) fabs(in[1].real - in[0].real);
        out[0] = '\0';
        int n = s / 3600 / 24;
        if (n > 1) sprintf(out + i, "%d days", n);
        else if (n > 0) strcpy(out + i, "1 day");
        i += strlen(out + i);
        n = (s / 3600) % 24;
        if ((n > 0) && (i > 0)) out[i++] = ' ';
        if (n > 1) sprintf(out + i, "%d hours", n);
        else if (n > 0) strcpy(out + i, "1 hour");
        i += strlen(out + i);
        n = (s / 60) % 60;
        if ((n > 0) && (i > 0)) out[i++] = ' ';
        if (n > 1) sprintf(out + i, "%d minutes", n);
        else if (n > 0) strcpy(out + i, "1 minute");
        i += strlen(out + i);
        n = s % 60;
        if ((n > 0) && (i > 0)) out[i++] = ' ';
        if (n > 1) sprintf(out + i, "%d seconds", n);
        else if (n > 0) strcpy(out + i, "1 second");
        i += strlen(out + i);
    }
    pplObjStr(&OUTPUT, 0, 1, out);
}

//! currentUnixTime - Read the current time, to a precision of better than a second
//! \return The current unix time, in seconds

static double currentUnixTime() {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return (double) time(NULL);
    return now.tv_sec + now.tv_nsec * 1e-9;
}

//! pplfunc_timenow - Implement the function time.now() in the time module, which returns a date object representing the
//! current time
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_timenow(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "now()";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    pplObjDate(&OUTPUT, 0, currentUnixTime());
    if (ppl_funcCheckOutput(&call)) return;
}

//! pplfunc_timestring - Implement the function time.string(t,<s>) in the time module, which returns a string
//! representation of the date object t; a format string may optionally be supplied
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_timestring(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    // The first argument must be a date, and the optional format and timezone strings
    char *FunctionDescription = "string(t,<s>,<timezone>)";
    char *format = NULL, *out = NULL, timezone[FNAME_LENGTH];
    double offset;
    // The first argument must be a date
    if (in[0].objType != PPLOBJ_DATE) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a date as its first argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    // The optional format must be a string
    if ((nArgs > 1) && (in[1].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a string as its second argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[1].objType]);
        return;
    }
    // The optional timezone must be a string
    if ((nArgs > 2) && (in[2].objType != PPLOBJ_STR)) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a string as its third argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[2].objType]);
        return;
    }

    // A buffer for the output string
    out = (char *) malloc(8192);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_funcError(status, errType, errText, ERR_MEMORY, "%s", "Out of memory.");
        return;
    }

    if (nArgs > 1) format = (char *) in[1].auxil; // Format specified
    else format = NULL; // Format not specified
    if (nArgs > 2) ppl_calendarTimezoneSet(c, 1, (char *) in[2].auxil);
    else ppl_calendarTimezoneSet(c, 0, NULL);
    ppl_calendarTimezoneOffset(c, in[0].real, timezone, &offset);
    ppl_dateString(c, out, 8192, in[0].real + offset, format, timezone, status, errText);
    ppl_calendarTimezoneUnset(c);
    // Report a date which cannot be expressed in the current calendar, or a bad format string
    if (*status) {
        *errType = ERR_NUMERICAL;
        return;
    }
    pplObjStr(&OUTPUT, 0, 1, out);
}

//! pplfunc_sleep - Implement the function time.sleep(t) in the time module, which sleeps for t seconds, or for time
//! period t if it has dimensions of time
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sleep(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    char *FunctionDescription = "sleep(t)";
    pplFuncCall call = {c, in, status, errType, errText, FunctionDescription};
    struct timespec waitperiod, waitperiod2; // A time.h timespec specifier for a 50ms nanosleep wait
    if (ppl_funcCheckUnit(&call, &in[0], "first", "a time", UNIT_TIME, 1)) return;
    waitperiod.tv_sec = floor(in[0].real);
    waitperiod.tv_nsec = fmod(in[0].real * 1e9, 1e9);
    for (int i = 0; i < 1e6; i++)
        if (!nanosleep(&waitperiod, &waitperiod2)) break;
        else waitperiod = waitperiod2;
    pplObjNum(&OUTPUT, 0, 0, 0);
}

//! pplfunc_sleepUntil - Implement the function time.sleepUntil(d) in the time module, which sleeps until the specified
//! date and time. Its argument should be a date object
//! \param [in] c - The Pyxplot context
//! \param [in|out] in - Array of the arguments passed to the function, in[0] to in[nArgs-1]. The return value
//! is written into in[-1] (OUTPUT).
//! \param [in] nArgs - The number of arguments passed to the function
//! \param [out] status - Set to one if an error occurred
//! \param [out] errType - Set to the type of error, one of the ERR_* constants, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void pplfunc_sleepUntil(ppl_context *c, pplObj *in, const int nArgs, int *status, int *errType, char *errText) {
    const double p = in[0].real - currentUnixTime();
    char *FunctionDescription = "sleepUntil(d)";
    struct timespec waitperiod, waitperiod2; // A time.h timespec specifier for a 50ms nanosleep wait
    // The argument must be a date
    if (in[0].objType != PPLOBJ_DATE) {
        ppl_funcError(status, errType, errText, ERR_TYPE,
                "The function %s requires a date as its argument; supplied argument had type <%s>.",
                FunctionDescription, pplObjTypeNames[in[0].objType]);
        return;
    }
    // Sleep until the date, if it is in the future, restarting the sleep if it is interrupted by a signal
    if (p > 0) {
        waitperiod.tv_sec = floor(p);
        waitperiod.tv_nsec = fmod(p * 1e9, 1e9);
        for (int i = 0; i < 1e6; i++)
            if (!nanosleep(&waitperiod, &waitperiod2)) break;
            else waitperiod = waitperiod2;
    }
    pplObjNum(&OUTPUT, 0, 0, 0);
}
