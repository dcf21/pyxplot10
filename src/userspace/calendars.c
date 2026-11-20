// calendars.c
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

#define _CALENDARS_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <gsl/gsl_math.h>

#include "userspace/calendars.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "settings/settingTypes.h"
#include "stringTools/strConstants.h"

// Routines for looking up the dates when the transition between the Julian calendar and the Gregorian calendar occurred

//! ppl_switchOverCalDate - Return the calendar dates of the transition between the Julian and Gregorian calendars,
//! for the calendar currently selected for input
//! \param [in] ct - The Pyxplot context
//! \param [out] LastJulian - The last day of the Julian calendar, expressed as yyyymmdd, or -HUGE_VAL if the Julian
//! calendar is never used, or HUGE_VAL if the Gregorian calendar is never used
//! \param [out] FirstGregorian - The first day of the Gregorian calendar, expressed as yyyymmdd, or +/- HUGE_VAL as
//! above

void ppl_switchOverCalDate(ppl_context *ct, double *LastJulian, double *FirstGregorian) {
    switch (ct->set->term_current.CalendarIn) {
        case SW_CALENDAR_GREGORIAN: {
            *LastJulian = -HUGE_VAL;
            *FirstGregorian = -HUGE_VAL;
            return;
        }
        case SW_CALENDAR_JULIAN: {
            *LastJulian = HUGE_VAL;
            *FirstGregorian = HUGE_VAL;
            return;
        }
        case SW_CALENDAR_BRITISH: {
            *LastJulian = 17520902.0;
            *FirstGregorian = 17520914.0;
            return;
        }
        case SW_CALENDAR_FRENCH: {
            *LastJulian = 15821209.0;
            *FirstGregorian = 15821220.0;
            return;
        }
        case SW_CALENDAR_CATHOLIC: {
            *LastJulian = 15821004.0;
            *FirstGregorian = 15821015.0;
            return;
        }
        case SW_CALENDAR_RUSSIAN: {
            *LastJulian = 19180131.0;
            *FirstGregorian = 19180214.0;
            return;
        }
        case SW_CALENDAR_GREEK: {
            *LastJulian = 19230215.0;
            *FirstGregorian = 19230301.0;
            return;
        }
        default:
            ppl_fatal(&ct->errcontext, __FILE__, __LINE__,
                      "Internal Error: Calendar option is set to an illegal setting.");
    }
}

//! ppl_switchOverJD - Return the Julian day number of the transition between the Julian and Gregorian calendars,
//! for the calendar currently selected for output
//! \param [in] ct - The Pyxplot context
//! \return The Julian day number of the first day of the Gregorian calendar, or -HUGE_VAL if the Julian calendar is
//! never used, or HUGE_VAL if the Gregorian calendar is never used

double ppl_switchOverJD(ppl_context *ct) {
    switch (ct->set->term_current.CalendarOut) {
        case SW_CALENDAR_GREGORIAN:
            return -HUGE_VAL;
        case SW_CALENDAR_JULIAN:
            return HUGE_VAL;
        case SW_CALENDAR_BRITISH:
            return 2361222.0;
        case SW_CALENDAR_FRENCH:
            return 2299227.0;
        case SW_CALENDAR_CATHOLIC:
            return 2299161.0;
        case SW_CALENDAR_RUSSIAN:
            return 2421639.0;
        case SW_CALENDAR_GREEK:
            return 2423480.0;
        default:
            ppl_fatal(&ct->errcontext, __FILE__, __LINE__,
                      "Internal Error: Calendar option is set to an illegal setting.");
    }
    return 0; // Never gets here
}

// Functions for looking up the names of the Nth calendar month and the Nth day of the week

//! ppl_getMonthName - Return the name of the Nth month of the year, in the calendar currently selected for output
//! \param [in] ct - The Pyxplot context
//! \param [in] i - The month number, where 1 is the first month of the year
//! \return The name of the month, or "???" if the month number is out of range

char *ppl_getMonthName(ppl_context *ct, const int i) {
    switch (ct->set->term_current.CalendarOut) {
        case SW_CALENDAR_HEBREW:
            switch (i) {
                case 1:
                    return "Tishri";
                case 2:
                    return "Heshvan";
                case 3:
                    return "Kislev";
                case 4:
                    return "Tevet";
                case 5:
                    return "Shevat";
                case 6:
                    return "Adar";
                case 7:
                    return "Veadar";
                case 8:
                    return "Nisan";
                case 9:
                    return "Iyar";
                case 10:
                    return "Sivan";
                case 11:
                    return "Tammuz";
                case 12:
                    return "Av";
                case 13:
                    return "Elul";
                default:
                    return "???";
            }
        case SW_CALENDAR_ISLAMIC:
            switch (i) {
                case 1:
                    return "Muharram";
                case 2:
                    return "Safar";
                case 3:
                    return "Rabi'al-Awwal";
                case 4:
                    return "Rabi'ath-Thani";
                case 5:
                    return "Jumada l-Ula";
                case 6:
                    return "Jumada t-Tania";
                case 7:
                    return "Rajab";
                case 8:
                    return "Sha'ban";
                case 9:
                    return "Ramadan";
                case 10:
                    return "Shawwal";
                case 11:
                    return "Dhu l-Qa'da";
                case 12:
                    return "Dhu l-Hijja";
                default:
                    return "???";
            }
        default:
            switch (i) {
                case 1:
                    return "January";
                case 2:
                    return "February";
                case 3:
                    return "March";
                case 4:
                    return "April";
                case 5:
                    return "May";
                case 6:
                    return "June";
                case 7:
                    return "July";
                case 8:
                    return "August";
                case 9:
                    return "September";
                case 10:
                    return "October";
                case 11:
                    return "November";
                case 12:
                    return "December";
                default:
                    return "???";
            }
    }
    return "???";
}

//! ppl_getWeekDayName - Return the name of the Nth day of the week
//! \param [in] ct - The Pyxplot context
//! \param [in] i - The day number, where 0 is Monday and 6 is Sunday. Negative values count back from Sunday.
//! \return The name of the day, or "???" if the day number is out of range

char *ppl_getWeekDayName(ppl_context *ct, int i) {
    if (i < 0) i += 7;
    switch (i) {
        case 0:
            return "Monday";
        case 1:
            return "Tuesday";
        case 2:
            return "Wednesday";
        case 3:
            return "Thursday";
        case 4:
            return "Friday";
        case 5:
            return "Saturday";
        case 6:
            return "Sunday";
    }
    return "???";
}

// Routines for converting between unix times and the Hebrew calendar

static int hebrewMonthLengths[6][13] = {
        {30, 29, 29, 29, 30, 29, 0, 30, 29, 30, 29, 30, 29}, // Deficient Common Year
        {30, 29, 30, 29, 30, 29, 0, 30, 29, 30, 29, 30, 29}, // Regular   Common Year
        {30, 30, 30, 29, 30, 29, 0, 30, 29, 30, 29, 30, 29}, // Complete  Common Year
        {30, 29, 29, 29, 30, 30, 29, 30, 29, 30, 29, 30, 29}, // Deficient Embolismic Year
        {30, 29, 30, 29, 30, 30, 29, 30, 29, 30, 29, 30, 29}, // Regular   Embolismic Year
        {30, 30, 30, 29, 30, 30, 29, 30, 29, 30, 29, 30, 29}}; // Complete  Embolismic Year

//! ppl_getHebrewNewYears - Compute the dates of three consecutive Hebrew new years, starting with the one which
//! falls in the year before a specified Gregorian year
//! \param [in] ct - The Pyxplot context
//! \param [in] GregYear - The Gregorian year
//! \param [out] YearNumbers - Array of three Hebrew year numbers
//! \param [out] JDs - Array of the three Julian day numbers on which the Hebrew years begin
//! \param [out] YearTypes - Array of the types of the first two Hebrew years, encoding their lengths in days

void ppl_getHebrewNewYears(const ppl_context *ct, const int GregYear, int *YearNumbers, double *JDs, int *YearTypes) {
    int i;
    long X, C, S, A, a, b, j, D, MONTH = 3;
    double Q, JD, r;

    for (i = 0; i < 3; i++) // Return THREE new year numbers and Julian Day numbers, and YearTypes for the first two.
    {
        X = GregYear - 1 + i;
        C = X / 100;
        S = (3 * C - 5) / 4;
        A = X + 3760;
        a = (12 * X + 12) % 19; // Work out the date of the passover in the Hebrew year A
        b = X % 4;
        Q = -1.904412361576 + 1.554241796621 * a + 0.25 * b - 0.003177794022 * X + S;
        j = (((long) Q) + 3 * X + 5 * b + 2 - S) % 7;
        r = Q - ((long) Q);

        if ((j == 2) || (j == 4) || (j == 6)) D = ((long) Q) + 23;
        else if ((j == 1) && (a > 6) && (r >= 0.632870370)) D = ((long) Q) + 24;
        else if ((j == 0) && (a > 11) && (r >= 0.897723765)) D = ((long) Q) + 23;
        else D = ((long) Q) + 22; // D is day when 15 Nisan falls after 1st March in the Gregorian year X

        b = (X / 400) - (X / 100) + (X / 4); // Gregorian calendar
        JD = 365.0 * X - 679004.0 + 2400000.5 + b + floor(30.6001 * (MONTH + 1)) + D;

        YearNumbers[i] = A + 1;
        JDs[i] = JD + 163; // 1 Tishri (New Year) for next year falls 163 days after 15 Nisan

        a = (A + 1) % 19;
        if ((a == 0) || (a == 3) || (a == 6) || (a == 8) || (a == 11) || (a == 14) || (a == 17)) YearTypes[i] = 3;
        else YearTypes[i] = 0;
    }

    // We've so far worked out whether years are Common or Embolismic. Now need to work out how many days there will be in each year.
    for (i = 0; i < 2; i++) {
        if (YearTypes[i] == 0) YearTypes[i] = JDs[i + 1] - JDs[i] - 353;
        else YearTypes[i] = 3 + JDs[i + 1] - JDs[i] - 383;

        if ((YearTypes[i] < 0) || (YearTypes[i] > 6)) YearTypes[i] = 0; // Something very bad has happened.
    }
    return;
}

//! ppl_hebrewToUnixTime - Convert a date in the Hebrew calendar into a unix time
//! \param [in] ct - The Pyxplot context
//! \param [in] year - The Hebrew year
//! \param [in] month - The month number, 1-13
//! \param [in] day - The day of the month
//! \param [in] hour - The hour of the day
//! \param [in] min - The minute
//! \param [in] sec - The second
//! \param [out] status - Set to one if the date is invalid
//! \param [out] errText - Character buffer into which to write an error message if the date is invalid
//! \return The unix time of the date

double ppl_hebrewToUnixTime(ppl_context *ct, int year, int month, const int day, const int hour, const int min,
                            const int sec, int *status, char *errText) {
    int i;
    int YearNumbers[3], YearTypes[3];
    double JD, DayFraction, JDs[3];

    if ((month < 1) || (month > 13)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied month number should be in the range 1-13.");
        return 0.0;
    }
    if ((year < 1)) {
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "Supplied year number must be positive for the Hebrew calendar; the calendar is undefined prior to 4760 BC, corresponding to Hebrew year AM 1.");
        return 0.0;
    }

    ppl_getHebrewNewYears(ct, year - 3760, YearNumbers, JDs, YearTypes);
    JD = JDs[0];
    for (i = 0; i < month - 1; i++) JD += hebrewMonthLengths[YearTypes[0]][i];
    if (day > hebrewMonthLengths[YearTypes[0]][i]) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Supplied day number in the Hebrew month of %s in the year AM %d must be in the range 1-%d.",
                          ppl_getMonthName(ct, month), year, hebrewMonthLengths[YearTypes[1]][i]);
        return 0.0;
    }
    JD += day - 1;
    JD = 86400.0 * (JD - 2440587.5);
    DayFraction = fabs(hour) * 3600 + fabs(min) * 60 + fabs(sec);
    return JD + DayFraction;
}

//! ppl_hebrewFromUnixTime - Convert a unix time into a date in the Hebrew calendar
//! \param [in] ct - The Pyxplot context
//! \param [in] UT - The unix time to convert
//! \param [out] year - The Hebrew year
//! \param [out] month - The month number, 1-13
//! \param [out] day - The day of the month
//! \param [in] status - Unused
//! \param [in] errText - Unused

void ppl_hebrewFromUnixTime(ppl_context *ct, const double UT, int *year, int *month, int *day, const int *status,
                            const char *errText) {
    long a, c, d, e, f;
    int i, j;
    int JulMon, JulYr;
    int YearNumbers[3], YearTypes[3];
    double JDs[3];
    double JD = (UT / 86400.0) + 2440587.5;

    // First work out date in Julian calendar
    a = JD +
            0.5; // Number of whole Julian days. b = Number of centuries since the Council of Nicaea. c = Julian Day number as if century leap years happened.
    c = a + 1524;
    d = (c - 122.1) / 365.25; // Number of 365.25 periods, starting the year at the end of February
    e = 365 * d + d / 4; // Number of days accounted for by these
    f = (c - e) / 30.6001; // Number of 30.6001 days periods (a.k.a. months) in remainder
    //JulDay = (int)floor(c-e-(int)(30.6001*f));
    JulMon = (int) floor(f - 1 - 12 * (f >= 14));
    JulYr = (int) floor(d - 4715 - (JulMon >= 3));

    ppl_getHebrewNewYears(ct, JulYr, YearNumbers, JDs, YearTypes);
    i = (JD < JDs[1]) ? 0 : 1;
    JD -= JDs[i];
    for (j = 0; j < 13; j++)
        if (JD >= hebrewMonthLengths[YearTypes[i]][j]) JD -= hebrewMonthLengths[YearTypes[i]][j];
        else break;
    if (year != NULL) *year = YearNumbers[i];
    if (month != NULL) *month = j + 1;
    if (day != NULL) *day = ((int) JD + 1);
    return;
}

// Routines for converting between unix times and the Islamic calendar

//! ppl_islamicToUnixTime - Convert a date in the Islamic calendar into a unix time
//! \param [in] ct - The Pyxplot context
//! \param [in] year - The Islamic year
//! \param [in] month - The month number, 1-12
//! \param [in] day - The day of the month
//! \param [in] hour - The hour of the day
//! \param [in] min - The minute
//! \param [in] sec - The second
//! \param [out] status - Set to one if the date is invalid
//! \param [out] errText - Character buffer into which to write an error message if the date is invalid
//! \return The unix time of the date

double ppl_islamicToUnixTime(const ppl_context *ct, const int year, int month, const int day, const int hour,
                             const int min, const int sec, int *status, char *errText) {
    long N, Q, R, A, W, Q1, Q2, G, K, E, J, X;
    double DayFraction;

    if ((month < 1) || (month > 12)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied month number should be in the range 1-12.");
        return 0.0;
    }
    if ((year < 1)) {
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "Supplied year number must be positive for the Islamic calendar; the calendar is undefined prior to AD 622 Jul 18, corresponding to AH 1 Muh 1.");
        return 0.0;
    }

    N = day + (long) (29.5001 * (month - 1) + 0.99);
    Q = year / 30;
    R = year % 30;
    A = (long) ((11 * R + 3) / 30);
    W = 404 * Q + 354 * R + 208 + A;
    Q1 = W / 1461;
    Q2 = W % 1461;
    G = 621 + 4 * ((long) (7 * Q + Q1));
    K = Q2 / 365.2422;
    E = 365.2422 * K;
    J = Q2 - E + N - 1;
    X = G + K;

    if ((J > 366) && (X % 4 == 0)) {
        J -= 366;
        X++;
    } else if ((J > 365) && (X % 4 > 0)) {
        J -= 365;
        X++;
    }

    // Julian day number of midnight at the start of the requested day, computed in double precision
    const double JDmidnight = floor(365.25 * (X - 1)) + 1721423 + J - 0.5;
    DayFraction = fabs(hour) * 3600 + fabs(min) * 60 + fabs(sec);
    return 86400.0 * (JDmidnight - 2440587.5) + DayFraction;
}

//! ppl_islamicFromUnixTime - Convert a unix time into a date in the Islamic calendar
//! \param [in] ct - The Pyxplot context
//! \param [in] UT - The unix time to convert
//! \param [out] year - The Islamic year
//! \param [out] month - The month number, 1-12
//! \param [out] day - The day of the month
//! \param [out] status - Set to one if the date precedes the start of the Islamic calendar
//! \param [out] errText - Character buffer into which to write an error message

void ppl_islamicFromUnixTime(const ppl_context *ct, const double UT, int *year, int *month, int *day, int *status,
                             char *errText) {
    long a, b, c, d, e, f;
    int JulDay, JulMon, JulYr;
    long W, N, A, B, C, C2, D, Q, R, J, K, O, H, JJ, CL, DL, S, m;
    double C1;
    const double JD = (UT / 86400.0) + 2440587.5;

    if (JD < 1948439.5) {
        *status = 1;
        ppl_snprintfTrunc(
                errText, SSTR_LENGTH,
                "Supplied year number must be positive for the Islamic calendar; the calendar is undefined prior to AD 622 Jul 18, corresponding to AH 1 Muh 1.");
        return;
    }

    // First work out date in Julian calendar
    a = JD +
            0.5; // Number of whole Julian days. b = Number of centuries since the Council of Nicaea. c = Julian Day number as if century leap years happened.
    b = 0;
    c = a + 1524;
    d = (c - 122.1) / 365.25; // Number of 365.25 periods, starting the year at the end of February
    e = 365 * d + d / 4; // Number of days accounted for by these
    f = (c - e) / 30.6001; // Number of 30.6001 days periods (a.k.a. months) in remainder
    JulDay = (int) floor(c - e - (int) (30.6001 * f));
    JulMon = (int) floor(f - 1 - 12 * (f >= 14));
    JulYr = (int) floor(d - 4715 - (JulMon >= 3));

    //alpha = (JD-1867216.25)/36524.25; // See pages 75-76 of "Astronomical Algorithms", by Jean Meeus

    //if (JD<2299161)  beta = (JD+0.5);
    //else             beta = (JD+0.5) + 1 + alpha - ((long)(alpha/4));

    c = (b - 122.1) / 365.25;
    d = 365.25 * c;
    e = (b - d) / 30.6001;

    W = 2 - (JulYr % 4 == 0);
    N = ((long) ((275 * JulMon) / 9)) - W * ((long) ((JulMon + 9) / 12)) + JulDay - 30;
    A = JulYr - 623;
    B = A / 4;
    C = A % 4;
    C1 = 365.2501 * C;
    C2 = C1;

    if (C1 - C2 > 0.5) C2++;

    D = 1461 * B + 170 + C2;
    Q = D / 10631;
    R = D % 10631;
    J = R / 354;
    K = R % 354;
    O = (11 * J + 14) / 30;
    H = 30 * Q + J + 1;
    JJ = K - O + N - 1;

    if (JJ > 354) {
        CL = H % 30;
        DL = (11 * CL + 3) % 30;
        if (DL < 19) {
            JJ -= 354;
            H++;
        } else if (DL > 18) {
            JJ -= 355;
            H++;
        }
        if (JJ == 0) {
            JJ = 355;
            H--;
        }
    }

    S = (JJ - 1) / 29.5;
    m = 1 + S;
    d = JJ - 29.5 * S;

    if (JJ == 355) {
        m = 12;
        d = 30;
    }

    if (year != NULL) *year = (int) H;
    if (month != NULL) *month = (int) m;
    if (day != NULL) *day = (int) d;
    return;
}

// Routines for converting between unix times and calendar dates in Gregorian and Julian calendars

//! ppl_toUnixTime - Convert a calendar date into a unix time, using the calendar currently selected for input
//! \param [in] ct - The Pyxplot context
//! \param [in] year - The year
//! \param [in] month - The month number, where 1 is the first month of the year
//! \param [in] day - The day of the month
//! \param [in] hour - The hour of the day
//! \param [in] min - The minute
//! \param [in] sec - The second
//! \param [out] status - Set to one if the date is invalid
//! \param [out] errText - Character buffer into which to write an error message if the date is invalid
//! \return The unix time of the date

double ppl_toUnixTime(ppl_context *ct, int year, int month, const int day, const int hour, const int min, const int sec,
                      int *status, char *errText) {
    double JD, DayFraction, LastJulian, FirstGregorian, ReqDate;
    int b;

    if ((year < -1e6) || (year > 1e6) || (!gsl_finite(year))) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied year is too big.");
        return 0.0;
    }
    if ((day < 1) || (day > 31)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied day number should be in the range 1-31.");
        return 0.0;
    }
    if ((hour < 0) || (hour > 23)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied hour number should be in the range 0-23.");
        return 0.0;
    }
    if ((min < 0) || (min > 59)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied minute number should be in the range 0-59.");
        return 0.0;
    }
    if ((sec < 0) || (sec > 59)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied second number should be in the range 0-59.");
        return 0.0;
    }

    if (ct->set->term_current.CalendarIn == SW_CALENDAR_HEBREW)
        return ppl_hebrewToUnixTime(ct, year, month, day, hour, min, sec, status, errText);
    else if (ct->set->term_current.CalendarIn == SW_CALENDAR_ISLAMIC)
        return ppl_islamicToUnixTime(ct, year, month, day, hour, min, sec, status, errText);

    if ((month < 1) || (month > 12)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied month number should be in the range 1-12.");
        return 0.0;
    }

    ppl_switchOverCalDate(ct, &LastJulian, &FirstGregorian);
    ReqDate = 10000.0 * year + 100 * month + day;

    if (month <= 2) {
        month += 12;
        year--;
    }

    if (ReqDate <= LastJulian) {
        b = -2 + ((year + 4716) / 4) - 1179;
    } // Julian calendar
    else if (ReqDate >= FirstGregorian) {
        b = (year / 400) - (year / 100) + (year / 4);
    } // Gregorian calendar
    else {
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "The requested date never happened in the %s calendar: it was lost in the transition from the Julian to the Gregorian calendar.",
                *(char **) ppl_fetchSettingName(&ct->errcontext, ct->set->term_current.CalendarIn, SW_CALENDAR_INT,
                                                (void *) SW_CALENDAR_STR, sizeof(char *)));
        return 0.0;
    }

    JD = 365.0 * year - 679004.0 + 2400000.5 + b + floor(30.6001 * (month + 1)) + day;
    JD = 86400.0 * (JD - 2440587.5);
    DayFraction = fabs(hour) * 3600 + fabs(min) * 60 + fabs(sec);
    return JD + DayFraction;
}

//! ppl_fromUnixTime - Convert a unix time into a calendar date, using the calendar currently selected for output
//! \param [in] ct - The Pyxplot context
//! \param [in] UT - The unix time to convert
//! \param [out] year - The year (optional)
//! \param [out] month - The month number, where 1 is the first month of the year (optional)
//! \param [out] day - The day of the month (optional)
//! \param [out] hour - The hour of the day (optional)
//! \param [out] min - The minute (optional)
//! \param [out] sec - The second (optional)
//! \param [out] status - Set to one if the time could not be converted
//! \param [out] errText - Character buffer into which to write an error message if the conversion failed

void ppl_fromUnixTime(ppl_context *ct, const double UT, int *year, int *month, int *day, int *hour, int *min,
                      double *sec, int *status, char *errText) {
    long a, b, c, d, e, f;
    double DayFraction;
    int temp;
    const double JD = (UT / 86400.0) + 2440587.5;
    if (month == NULL) month = &temp; // Dummy placeholder, since we need month later in the calculation

    if ((JD < -1e8) || (JD > 1e8) || (!gsl_finite(JD))) {
        *status = 1;
        ppl_snprintfTrunc(errText, SSTR_LENGTH, "Supplied unix time is too big.");
        return;
    }

    // Work out hours, minutes and seconds. Julian days begin at noon, so days begin at midnight on JD+0.5. The time
    // of day is worked out from the unix time, which is more precise than the Julian date (which is a large number).
    DayFraction = UT - 86400.0 * floor(UT / 86400.0); // Seconds since midnight
    if (hour != NULL) *hour = (int) floor(DayFraction / 3600);
    if (min != NULL) *min = (int) floor(fmod(DayFraction / 60, 60));
    if (sec != NULL) *sec = fmod(DayFraction, 60);

    // Now work out calendar date
    if (ct->set->term_current.CalendarOut == SW_CALENDAR_HEBREW)
        return ppl_hebrewFromUnixTime(ct, UT, year, month, day, status, errText);
    else if (ct->set->term_current.CalendarOut == SW_CALENDAR_ISLAMIC)
        return ppl_islamicFromUnixTime(ct, UT, year, month, day, status, errText);

    a = JD +
            0.5; // Number of whole Julian days. b = Number of centuries since the Council of Nicaea. c = Julian Day number as if century leap years happened.
    if (a < ppl_switchOverJD(ct)) {
        b = 0;
        c = a + 1524;
    } // Julian calendar
    else {
        b = (a - 1867216.25) / 36524.25;
        c = a + b - (b / 4) + 1525;
    } // Gregorian calendar
    d = (c - 122.1) / 365.25; // Number of 365.25 periods, starting the year at the end of February
    e = 365 * d + d / 4; // Number of days accounted for by these
    f = (c - e) / 30.6001; // Number of 30.6001 days periods (a.k.a. months) in remainder
    if (day != NULL) *day = (int) floor(c - e - (int) (30.6001 * f));
    *month = (int) floor(f - 1 - 12 * (f >= 14));
    if (year != NULL) *year = (int) floor(d - 4715 - (*month >= 3));
    return;
}

// String representations of dates

//! ppl_dateString - Render a unix time as a string, using a strftime-like format string
//! \param [in] ct - The Pyxplot context
//! \param [out] out - Character buffer into which to write the string
//! \param [in] outLen - The size of the buffer <out>
//! \param [in] UT - The unix time to render
//! \param [in] format - The format string, or NULL to use the default format "%a %Y %b %d %H:%M:%S %Z"
//! \param [in] timezone - The name of the timezone in which to render the time, or NULL to use the default
//! \param [out] status - Set to one if the time could not be rendered
//! \param [out] errText - Character buffer into which to write an error message if rendering failed

void ppl_dateString(ppl_context *ct, char *out, const size_t outLen, double UT, const char *format,
                    const char *timezone, int *status, char *errText) {
    int j = 0, k = 0;
    int year, month, day, hour, min;
    double sec;

    if (format == NULL) format = "%a %Y %b %d %H:%M:%S %Z";
    *status = 0;
    ppl_fromUnixTime(ct, UT, &year, &month, &day, &hour, &min, &sec, status, errText);
    if (*status) return;

    // Loop through format string making substitutions
    for (j = 0; format[j] != '\0'; j++) {
        if (format[j] != '%') {
            ppl_strAppendChar(out, outLen, &k, format[j]);
            continue;
        }
        switch (format[j + 1]) {
            case '%':
                ppl_strAppendf(out, outLen, &k, "%%");
                break;
            case 'a':
                ppl_strAppendf(out, outLen, &k, "%.3s", ppl_getWeekDayName(ct, floor(fmod(UT / 3600 / 24 + 3, 7))));
                break;
            case 'A':
                ppl_strAppendf(out, outLen, &k, "%s", ppl_getWeekDayName(ct, floor(fmod(UT / 3600 / 24 + 3, 7))));
                break;
            case 'b':
                ppl_strAppendf(out, outLen, &k, "%.3s", ppl_getMonthName(ct, month));
                break;
            case 'B':
                ppl_strAppendf(out, outLen, &k, "%s", ppl_getMonthName(ct, month));
                break;
            case 'C':
                ppl_strAppendf(out, outLen, &k, "%d", (year / 100) + 1);
                break;
            case 'd':
                ppl_strAppendf(out, outLen, &k, "%d", day);
                break;
            case 'H':
                ppl_strAppendf(out, outLen, &k, "%02d", hour);
                break;
            case 'I':
                ppl_strAppendf(out, outLen, &k, "%02d", ((hour - 1) % 12) + 1);
                break;
            case 'k':
                ppl_strAppendf(out, outLen, &k, "%d", hour);
                break;
            case 'l':
                ppl_strAppendf(out, outLen, &k, "%d", ((hour - 1) % 12) + 1);
                break;
            case 'm':
                ppl_strAppendf(out, outLen, &k, "%02d", month);
                break;
            case 'M':
                ppl_strAppendf(out, outLen, &k, "%02d", min);
                break;
            case 'p':
                ppl_strAppendf(out, outLen, &k, "%s", (hour < 12) ? "am" : "pm");
                break;
            case 'S':
                ppl_strAppendf(out, outLen, &k, "%02d", (int) sec);
                break;
            case 'y':
                ppl_strAppendf(out, outLen, &k, "%d", year % 100);
                break;
            case 'Y':
                ppl_strAppendf(out, outLen, &k, "%d", year);
                break;
            case 'Z':
                ppl_strAppendf(out, outLen, &k, "%s", timezone);
                break;
            default: {
                *status = 1;
                ppl_snprintfTrunc(
                        errText, SSTR_LENGTH,
                        "Format string supplied to convert date to string contains unrecognised substitution token '%%%c'.",
                        format[j + 1]);
                return;
            }
        }
        j++;
        k += strlen(out + k);
    }
    out[k] = '\0'; // Null terminate string
}

//! ppl_timeDiffStr - Render the time interval between two unix times as a string, using a format string in which
//! tokens such as %d and %H are replaced by the number of days and hours in the interval
//! \param [in] ct - The Pyxplot context
//! \param [out] out - Character buffer into which to write the string
//! \param [in] outLen - The size of the buffer <out>
//! \param [in] UT1 - The unix time at the start of the interval
//! \param [in] UT2 - The unix time at the end of the interval
//! \param [in] format - The format string
//! \param [out] status - Set to one if the format string was invalid
//! \param [out] errText - Character buffer into which to write an error message

void ppl_timeDiffStr(const ppl_context *ct, char *out, const size_t outLen, const double UT1, const double UT2,
                     const char *format, int *status, char *errText) {
    int j = 0, k = 0;
    long gapYears, gapDays, gapHours, gapMinutes, gapSeconds;

    gapYears = (UT2 - UT1) / 365 / 3600 / 24;
    gapDays = (UT2 - UT1) / 3600 / 24;
    gapHours = (UT2 - UT1) / 3600;
    gapMinutes = (UT2 - UT1) / 60;
    gapSeconds = (UT2 - UT1);

    // Loop through format string making substitutions
    for (j = 0; format[j] != '\0'; j++) {
        if (format[j] != '%') {
            ppl_strAppendChar(out, outLen, &k, format[j]);
            continue;
        }
        switch (format[j + 1]) {
            case '%':
                ppl_strAppendf(out, outLen, &k, "%%");
                break;
            case 'Y':
                ppl_strAppendf(out, outLen, &k, "%ld", gapYears);
                break;
            case 'D':
                ppl_strAppendf(out, outLen, &k, "%ld", gapDays);
                break;
            case 'd':
                ppl_strAppendf(out, outLen, &k, "%ld", gapDays % 365);
                break;
            case 'H':
                ppl_strAppendf(out, outLen, &k, "%ld", gapHours);
                break;
            case 'h':
                ppl_strAppendf(out, outLen, &k, "%ld", gapHours % 24);
                break;
            case 'M':
                ppl_strAppendf(out, outLen, &k, "%ld", gapMinutes);
                break;
            case 'm':
                ppl_strAppendf(out, outLen, &k, "%ld", gapMinutes % 60);
                break;
            case 'S':
                ppl_strAppendf(out, outLen, &k, "%ld", gapSeconds);
                break;
            case 's':
                ppl_strAppendf(out, outLen, &k, "%ld", gapSeconds % 60);
                break;
            default: {
                *status = 1;
                ppl_snprintfTrunc(
                        errText, LSTR_LENGTH,
                        "Format string supplied to convert time interval to string contains unrecognised substitution token '%%%c'.",
                        format[j + 1]);
                return;
            }
        }
        j++;
        k += strlen(out + k);
    }
    out[k] = '\0'; // Null terminate string
    return;
}

//! ppl_calendarTimezoneSet - Set the TZ environment variable, to select the timezone used by the C library's time
//! functions
//! \param [in] ct - The Pyxplot context
//! \param [in] specified - If true, use the timezone <tz>; otherwise use the timezone in Pyxplot's settings
//! \param [in] tz - The name of the timezone to use, if <specified> is true

void ppl_calendarTimezoneSet(const ppl_context *ct, const int specified, const char *tz) {
    if (specified) setenv("TZ", tz, 1);
    else setenv("TZ", ct->set->term_current.timezone, 1);
    return;
}

//! ppl_calendarTimezoneUnset - Reset the TZ environment variable to UTC
//! \param [in] ct - The Pyxplot context

void ppl_calendarTimezoneUnset(const ppl_context *ct) {
    setenv("TZ", "UTC", 1);
    return;
}

//! ppl_calendarTimezoneOffset - Look up the offset of the current timezone (set with ppl_calendarTimezoneSet())
//! from UTC at a particular time
//! \param [in] ct - The Pyxplot context
//! \param [in] unixTime - The unix time at which to compute the offset
//! \param [out] tzNameOut - Character buffer into which to write the abbreviated name of the timezone (optional)
//! \param [out] offset - Set to the offset of local time from UTC, in seconds (optional)

void ppl_calendarTimezoneOffset(const ppl_context *ct, double unixTime, char *tzNameOut, double *offset) {
    struct tm *t;
    int out = 0, i;
    for (i = 0; i < 2; i++) {
        time_t in = (time_t) (unixTime - out);
        t = localtime(&in);
        out = (int) t->tm_gmtoff;
    }
    if (offset != NULL) *offset = out;
    if (tzNameOut != NULL) strcpy(tzNameOut, t->tm_zone);
    return;
}
