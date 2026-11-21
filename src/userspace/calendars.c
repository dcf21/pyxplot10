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

// Converts between calendar dates and the unix times in which Pyxplot stores dates, in the Julian, Gregorian, Hebrew
// and Islamic calendars, and in the historical calendars of countries which switched from the Julian to the Gregorian
// calendar on different dates (e.g. Britain in 1752). It also formats dates as strings, describes time intervals, and
// handles timezones. Used by the time module and the methods of date objects.

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
    // The last day of the Julian calendar and the first of the Gregorian calendar, as YYYYMMDD, in each country's
    // calendar. Purely Julian or Gregorian calendars never switch.
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
    // The Julian Day number of the first day of the Gregorian calendar in each country
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
    // The Hebrew and Islamic calendars have their own months; the others use the months of the Julian and Gregorian
    // calendars
    switch (ct->set->term_current.CalendarOut) {
        case SW_CALENDAR_HEBREW:
            // The months of the Hebrew calendar; Veadar is the extra month of embolismic (leap) years
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
            // The months of the Islamic calendar
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
            // The months of the Julian and Gregorian calendars
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
    // Days of the week are numbered from Monday
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

// The lengths of the months of the six types of Hebrew year, which differ in the lengths of Heshvan and Kislev, and in
// whether they have the extra month Veadar (length 0 in common years)
static int hebrewMonthLengths[6][13] = {
        {30, 29, 29, 29, 30, 29, 0, 30, 29, 30, 29, 30, 29}, // Deficient Common Year
        {30, 29, 30, 29, 30, 29, 0, 30, 29, 30, 29, 30, 29}, // Regular   Common Year
        {30, 30, 30, 29, 30, 29, 0, 30, 29, 30, 29, 30, 29}, // Complete  Common Year
        {30, 29, 29, 29, 30, 30, 29, 30, 29, 30, 29, 30, 29}, // Deficient Embolismic Year
        {30, 29, 30, 29, 30, 30, 29, 30, 29, 30, 29, 30, 29}, // Regular   Embolismic Year
        {30, 30, 30, 29, 30, 30, 29, 30, 29, 30, 29, 30, 29}}; // Complete  Embolismic Year

//! ppl_floorDiv - Integer division rounding towards minus infinity, unlike C's division, which rounds towards zero
//! \param [in] a - The dividend
//! \param [in] b - The divisor, which must be positive
//! \return floor(a / b)

static long ppl_floorDiv(const long a, const long b) { return (a >= 0) ? (a / b) : -((-a + b - 1) / b); }

//! ppl_floorMod - The remainder of integer division rounding towards minus infinity, which unlike C's % operator, is
//! never negative
//! \param [in] a - The dividend
//! \param [in] b - The divisor, which must be positive
//! \return a - b * floor(a / b), in the range 0 to b-1

static long ppl_floorMod(const long a, const long b) { return a - b * ppl_floorDiv(a, b); }

//! ppl_dayNumberToCalendar - Convert a Julian Day number into a date in the Julian or Gregorian calendar, using the
//! algorithm of Meeus, Astronomical Algorithms (1998), Chapter 7. Meeus requires a positive Julian Date, because it
//! takes the integer parts of quantities by truncation; taking them with floor() extends it to all dates.
//! \param [in] a - The Julian Day number, i.e. the Julian Date at noon on the required day
//! \param [in] gregorian - If true, use the (proleptic) Gregorian calendar; otherwise use the Julian calendar
//! \param [out] year - The year, numbered astronomically, so that 1 BC is year 0 (optional)
//! \param [out] month - The month number, 1-12
//! \param [out] day - The day of the month (optional)

static void ppl_dayNumberToCalendar(const long a, const int gregorian, int *year, int *month, int *day) {
    long b, c;
    // Meeus's algorithm: count the days from a March 1, so that leap days fall at the end of each year
    if (!gregorian) {
        // c = the day number, offset so that it counts from a March 1 before the start of the Julian period
        c = a + 1524;
    } else {
        // b = the number of centuries since AD 400, less those century years which were leap years; c is then the day
        // number as if every century year had been a leap year
        b = (long) floor((a - 1867216.25) / 36524.25);
        c = a + b - ppl_floorDiv(b, 4) + 1525;
    }
    long d = (long) floor((c - 122.1) / 365.25); // Number of 365.25 day periods, starting the year at the start of March
    long e = (long) floor(365.25 * d);           // Number of days accounted for by these
    long f = (long) floor((c - e) / 30.6001);    // Number of 30.6001 day periods (a.k.a. months) in the remainder
    if (day != NULL) *day = (int) (c - e - (long) floor(30.6001 * f));
    *month = (int) (f - 1 - 12 * (f >= 14));
    if (year != NULL) *year = (int) (d - 4715 - (*month >= 3));
}

//! ppl_getHebrewNewYears - Compute the dates of three consecutive Hebrew new years, starting with the one which
//! falls in the year before a specified Gregorian year
//! \param [in] ct - The Pyxplot context
//! \param [in] GregYear - The Gregorian year
//! \param [out] YearNumbers - Array of three Hebrew year numbers
//! \param [out] JDs - Array of the three Julian day numbers on which the Hebrew years begin
//! \param [out] YearTypes - Array of the types of the first two Hebrew years, encoding their lengths in days

void ppl_getHebrewNewYears(const ppl_context *ct, const int GregYear, int *YearNumbers, double *JDs, int *YearTypes) {
    // This is Gauss's algorithm for the date of Passover. X is the Gregorian year; C its century; S the number of
    // Gregorian leap days dropped since the Julian calendar; A the Hebrew year; a the year's position in the 19-year
    // Metonic cycle; Q the date of Passover in March, as a real number; j the day of the week on which it falls; and r
    // its fractional part.
    int i;
    long X, C, S, A, a, b, j, D, Qint, MONTH = 3;
    double Q, JD, r;

    for (i = 0; i < 3; i++) // Return THREE new year numbers and Julian Day numbers, and YearTypes for the first two.
    {
        X = GregYear - 1 + i;
        C = ppl_floorDiv(X, 100);
        S = ppl_floorDiv(3 * C - 5, 4);
        A = X + 3760;
        a = ppl_floorMod(12 * X + 12, 19); // Work out the date of the passover in the Hebrew year A
        b = ppl_floorMod(X, 4);
        Q = -1.904412361576 + 1.554241796621 * a + 0.25 * b - 0.003177794022 * X + S;
        Qint = (long) floor(Q);
        j = ppl_floorMod(Qint + 3 * X + 5 * b + 2 - S, 7);
        r = Q - Qint;

        // The rules of postponement, which stop certain festivals falling on certain days of the week, give the day in
        // March of 15 Nisan
        if ((j == 2) || (j == 4) || (j == 6)) D = Qint + 23;
        else if ((j == 1) && (a > 6) && (r >= 0.632870370)) D = Qint + 24;
        else if ((j == 0) && (a > 11) && (r >= 0.897723765)) D = Qint + 23;
        else D = Qint + 22; // D is day when 15 Nisan falls after 1st March in the Gregorian year X

        b = ppl_floorDiv(X, 400) - ppl_floorDiv(X, 100) + ppl_floorDiv(X, 4); // Gregorian calendar
        JD = 365.0 * X - 679004.0 + 2400000.5 + b + floor(30.6001 * (MONTH + 1)) + D;

        // Tishri 1, the new year of the following Hebrew year, falls 163 days after 15 Nisan
        YearNumbers[i] = A + 1;
        JDs[i] = JD + 163; // 1 Tishri (New Year) for next year falls 163 days after 15 Nisan

        // Years 3, 6, 8, 11, 14, 17 and 19 of each 19-year cycle are embolismic: they have an extra month
        a = ppl_floorMod(A + 1, 19);
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
    double JDs[3];

    // Hebrew years have up to 13 months
    if ((month < 1) || (month > 13)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied month number should be in the range 1-13.");
        return 0.0;
    }
    // The Hebrew calendar starts at AM 1
    if ((year < 1)) {
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "Supplied year number must be positive for the Hebrew calendar; the calendar is undefined prior to 3761 BC, corresponding to Hebrew year AM 1.");
        return 0.0;
    }

    // Start at the new year, and add the lengths of the months before the one requested
    ppl_getHebrewNewYears(ct, year - 3760, YearNumbers, JDs, YearTypes);
    double JD = JDs[0];
    for (i = 0; i < month - 1; i++) JD += hebrewMonthLengths[YearTypes[0]][i];
    // The day must exist in that month
    if (day > hebrewMonthLengths[YearTypes[0]][i]) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH,
                          "Supplied day number in the Hebrew month of %s in the year AM %d must be in the range 1-%d.",
                          ppl_getMonthName(ct, month), year, hebrewMonthLengths[YearTypes[0]][i]);
        return 0.0;
    }
    JD += day - 1;
    JD = 86400.0 * (JD - 2440587.5);
    double DayFraction = fabs(hour) * 3600 + fabs(min) * 60 + fabs(sec);
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

void ppl_hebrewFromUnixTime(ppl_context *ct, const double UT, int *year, int *month, int *day, int *status,
                            char *errText) {
    int j;
    int JulMon, JulYr;
    int YearNumbers[3], YearTypes[3];
    double JDs[3];
    double JD = (UT / 86400.0) + 2440587.5;

    // Dates before Tishri 1 AM 1 cannot be expressed in the Hebrew calendar
    if (JD < 347997.5) {
        *status = 1;
        ppl_snprintfTrunc(
                errText, SSTR_LENGTH,
                "The Hebrew calendar is undefined prior to 3761 BC Oct 7, corresponding to AM 1 Tishri 1.");
        return;
    }

    // First work out the year in the Julian calendar
    ppl_dayNumberToCalendar((long) floor(UT / 86400.0) + 2440588, 0, &JulYr, &JulMon, NULL);

    // Find the Hebrew new years either side of the date, and count through the months of the year in which it falls
    ppl_getHebrewNewYears(ct, JulYr, YearNumbers, JDs, YearTypes);
    int i = (JD < JDs[1]) ? 0 : 1;
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

    // Islamic years have 12 months
    if ((month < 1) || (month > 12)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied month number should be in the range 1-12.");
        return 0.0;
    }
    // The Islamic calendar starts at AH 1
    if ((year < 1)) {
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "Supplied year number must be positive for the Islamic calendar; the calendar is undefined prior to AD 622 Jul 18, corresponding to AH 1 Muh 1.");
        return 0.0;
    }

    // The algorithm of Jean Meeus, Astronomical Algorithms, for the Julian date of an Islamic date: N is the day of the
    // Islamic year; the calendar repeats every 30 years, of which 11 are leap years; X is the Julian year and J the day
    // within it
    long N = day + (long) (29.5001 * (month - 1) + 0.99);
    long Q = year / 30;
    long R = year % 30;
    long A = (long) ((11 * R + 3) / 30);
    long W = 404 * Q + 354 * R + 208 + A;
    long Q1 = W / 1461;
    long Q2 = W % 1461;
    long G = 621 + 4 * ((long) (7 * Q + Q1));
    long K = Q2 / 365.2422;
    long E = 365.2422 * K;
    long J = Q2 - E + N - 1;
    long X = G + K;

    // Wrap past the end of a Julian year
    if ((J > 366) && (X % 4 == 0)) {
        J -= 366;
        X++;
    } else if ((J > 365) && (X % 4 > 0)) {
        J -= 365;
        X++;
    }

    // Julian day number of midnight at the start of the requested day, computed in double precision
    const double JDmidnight = floor(365.25 * (X - 1)) + 1721423 + J - 0.5;
    double DayFraction = fabs(hour) * 3600 + fabs(min) * 60 + fabs(sec);
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
    int JulDay, JulMon, JulYr;
    long CL, DL;
    const double JD = (UT / 86400.0) + 2440587.5;

    // Dates before AH 1 Muharram 1 cannot be expressed in the Islamic calendar
    if (JD < 1948439.5) {
        *status = 1;
        ppl_snprintfTrunc(
                errText, SSTR_LENGTH,
                "Supplied year number must be positive for the Islamic calendar; the calendar is undefined prior to AD 622 Jul 18, corresponding to AH 1 Muh 1.");
        return;
    }

    // First work out the date in the Julian calendar
    ppl_dayNumberToCalendar((long) floor(UT / 86400.0) + 2440588, 0, &JulYr, &JulMon, &JulDay);

    // Meeus's algorithm for the Islamic date: N is the day of the Julian year; A counts Julian years since 623, and B
    // and C split them into four-year cycles; D is then a day count from the start of the Islamic calendar, which
    // repeats every 10631 days (30 years); Q counts these cycles, J the years within one, and K the day within that
    // year
    long W = 2 - (JulYr % 4 == 0);
    long N = ((long) ((275 * JulMon) / 9)) - W * ((long) ((JulMon + 9) / 12)) + JulDay - 30;
    long A = JulYr - 623;
    long B = A / 4;
    long C = A % 4;
    double C1 = 365.2501 * C;
    long C2 = C1;

    // Round to the nearest day
    if (C1 - C2 > 0.5) C2++;

    // H is the Islamic year and JJ the day within it
    long D = 1461 * B + 170 + C2;
    long Q = D / 10631;
    long R = D % 10631;
    long J = R / 354;
    long K = R % 354;
    long O = (11 * J + 14) / 30;
    long H = 30 * Q + J + 1;
    long JJ = K - O + N - 1;

    // Correct for days which run past the end of a 354-day year, or of a 355-day leap year
    if (JJ > 354) {
        CL = H % 30;
        DL = (11 * CL + 3) % 30;
        // Common years have 354 days, and leap years 355
        if (DL < 19) {
            JJ -= 354;
            H++;
        } else if (DL > 18) {
            JJ -= 355;
            H++;
        }
        // The last day of a leap year
        if (JJ == 0) {
            JJ = 355;
            H--;
        }
    }

    // Months alternate between 30 and 29 days, so the month and day follow from the day of the year
    long S = (JJ - 1) / 29.5;
    long m = 1 + S;
    long d = JJ - 29.5 * S;

    // The leap day is the 30th of the last month
    if (JJ == 355) {
        m = 12;
        d = 30;
    }

    // Return the date
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
    double LastJulian, FirstGregorian;
    int b;

    // Check that each component of the date is in range
    if ((year < -1e6) || (year > 1e6) || (!gsl_finite(year))) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied year is too big.");
        return 0.0;
    }
    // Days of the month
    if ((day < 1) || (day > 31)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied day number should be in the range 1-31.");
        return 0.0;
    }
    // Hours
    if ((hour < 0) || (hour > 23)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied hour number should be in the range 0-23.");
        return 0.0;
    }
    // Minutes
    if ((min < 0) || (min > 59)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied minute number should be in the range 0-59.");
        return 0.0;
    }
    // Seconds
    if ((sec < 0) || (sec > 59)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied second number should be in the range 0-59.");
        return 0.0;
    }

    // The Hebrew and Islamic calendars have their own conversions
    if (ct->set->term_current.CalendarIn == SW_CALENDAR_HEBREW)
        return ppl_hebrewToUnixTime(ct, year, month, day, hour, min, sec, status, errText);
    else if (ct->set->term_current.CalendarIn == SW_CALENDAR_ISLAMIC)
        return ppl_islamicToUnixTime(ct, year, month, day, hour, min, sec, status, errText);

    // Julian and Gregorian years have 12 months
    if ((month < 1) || (month > 12)) {
        *status = 1;
        ppl_snprintfTrunc(errText, LSTR_LENGTH, "Supplied month number should be in the range 1-12.");
        return 0.0;
    }

    // Decide whether the date falls in the Julian or Gregorian part of the calendar, comparing dates as YYYYMMDD
    ppl_switchOverCalDate(ct, &LastJulian, &FirstGregorian);
    double ReqDate = 10000.0 * year + 100 * month + day;

    // Count the year from March, so that leap days fall at its end
    if (month <= 2) {
        month += 12;
        year--;
    }

    // b is the number of leap days, less a constant
    if (ReqDate <= LastJulian) {
        b = -2 + ppl_floorDiv(year + 4716, 4) - 1179;
    } // Julian calendar
    else if (ReqDate >= FirstGregorian) {
        b = ppl_floorDiv(year, 400) - ppl_floorDiv(year, 100) + ppl_floorDiv(year, 4);
    } // Gregorian calendar
    // Dates which fall in the gap between the two calendars never existed
    else {
        *status = 1;
        ppl_snprintfTrunc(
                errText, LSTR_LENGTH,
                "The requested date never happened in the %s calendar: it was lost in the transition from the Julian to the Gregorian calendar.",
                *(char **) ppl_fetchSettingName(&ct->errcontext, ct->set->term_current.CalendarIn, SW_CALENDAR_INT,
                                                (void *) SW_CALENDAR_STR, sizeof(char *)));
        return 0.0;
    }

    // The Modified Julian Date of the day (as in Meeus's algorithm), converted to a Julian Date and then to a unix time
    double JD = 365.0 * year - 679004.0 + 2400000.5 + b + floor(30.6001 * (month + 1)) + day;
    JD = 86400.0 * (JD - 2440587.5);
    double DayFraction = fabs(hour) * 3600 + fabs(min) * 60 + fabs(sec);
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
    int temp;
    const double JD = (UT / 86400.0) + 2440587.5;
    if (month == NULL) month = &temp; // Dummy placeholder, since we need month later in the calculation

    // Reject absurdly large dates
    if ((JD < -1e8) || (JD > 1e8) || (!gsl_finite(JD))) {
        *status = 1;
        ppl_snprintfTrunc(errText, SSTR_LENGTH, "Supplied unix time is too big.");
        return;
    }

    // Work out hours, minutes and seconds. Julian days begin at noon, so days begin at midnight on JD+0.5. The time
    // of day is worked out from the unix time, which is more precise than the Julian date (which is a large number).
    double DayFraction = UT - 86400.0 * floor(UT / 86400.0); // Seconds since midnight
    if (hour != NULL) *hour = (int) floor(DayFraction / 3600);
    if (min != NULL) *min = (int) floor(fmod(DayFraction / 60, 60));
    if (sec != NULL) *sec = fmod(DayFraction, 60);

    // Now work out calendar date
    if (ct->set->term_current.CalendarOut == SW_CALENDAR_HEBREW)
        return ppl_hebrewFromUnixTime(ct, UT, year, month, day, status, errText);
    else if (ct->set->term_current.CalendarOut == SW_CALENDAR_ISLAMIC)
        return ppl_islamicFromUnixTime(ct, UT, year, month, day, status, errText);

    // a = the number of the day (the Julian Date at noon on that day), counted from midnight so that it matches the
    // time of day worked out above
    long a = (long) floor(UT / 86400.0) + 2440588;
    ppl_dayNumberToCalendar(a, a >= ppl_switchOverJD(ct), year, month, day);
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

    // The default format, e.g. "Mon 2024 Jan 1 12:00:00 UTC"
    if (format == NULL) format = "%a %Y %b %d %H:%M:%S %Z";
    *status = 0;
    ppl_fromUnixTime(ct, UT, &year, &month, &day, &hour, &min, &sec, status, errText);
    if (*status) return;

    // Loop through format string making substitutions
    for (j = 0; format[j] != '\0'; j++) {
        // Characters other than % are copied as they are
        if (format[j] != '%') {
            ppl_strAppendChar(out, outLen, &k, format[j]);
            continue;
        }
        // Substitute each token, following the conventions of strftime(). The unix epoch was a Thursday, which is day 3
        // counting from Monday.
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
                ppl_strAppendf(out, outLen, &k, "%02d", ((hour + 11) % 12) + 1);
                break;
            case 'k':
                ppl_strAppendf(out, outLen, &k, "%d", hour);
                break;
            case 'l':
                ppl_strAppendf(out, outLen, &k, "%d", ((hour + 11) % 12) + 1);
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

    // The interval in each unit, truncated to whole units; years are taken to have 365 days
    long gapYears = (UT2 - UT1) / 365 / 3600 / 24;
    long gapDays = (UT2 - UT1) / 3600 / 24;
    long gapHours = (UT2 - UT1) / 3600;
    long gapMinutes = (UT2 - UT1) / 60;
    long gapSeconds = (UT2 - UT1);

    // Loop through format string making substitutions
    for (j = 0; format[j] != '\0'; j++) {
        // Characters other than % are copied as they are
        if (format[j] != '%') {
            ppl_strAppendChar(out, outLen, &k, format[j]);
            continue;
        }
        // Substitute each token: upper-case letters give the whole interval in that unit, lower-case ones the remainder
        // after the next larger unit
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
    // Find the offset of the timezone from UTC at the given local time; this is iterated, since the offset depends on
    // the time, e.g. across a change to daylight saving time
    for (i = 0; i < 2; i++) {
        time_t in = (time_t) (unixTime - out);
        t = localtime(&in);
        out = (int) t->tm_gmtoff;
    }
    if (offset != NULL) *offset = out;
    if (tzNameOut != NULL) strcpy(tzNameOut, t->tm_zone);
    return;
}
