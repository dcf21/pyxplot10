// asciidouble.c
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

// Low-level string handling used throughout Pyxplot: parsing numbers from strings, rendering numbers as text, reading
// lines from files, splitting strings into words, comparing strings, and handling UTF-8. It depends on nothing else in
// Pyxplot except the global cancellation flag, so it is used by every other part of the code, including the helper
// programs.

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <wctype.h>

#include "settings/settings.h"

#include <gsl/gsl_math.h>

//! ppl_getFloat - Extract a floating point number from a string
//! \param [in] str The input string
//! \param [out] Nchars Return the number of characters occupied by the float returned (optional)
//! \return The floating point value extracted

double ppl_getFloat(const char *str, int *Nchars) {
    // The value accumulated so far, and the number of digits read after the decimal point
    double accumulator = 0;
    int decimals = 0;
    unsigned char past_decimal_point = 0;
    unsigned char negative = 0;
    int pos = 0;
    int pos2 = 0;

    // The strings "nan" and "NAN", followed by whitespace or the end of the string, mean not-a-number
    if ((str[pos + 0] == 'n') && (str[pos + 1] == 'a') && (str[pos + 2] == 'n') && (str[pos + 3] >= '\0') &&
        ((unsigned char) str[pos + 3] <= ' ')) {
        if (Nchars != NULL) *Nchars = 3;
        return GSL_NAN;
    }
    if ((str[pos + 0] == 'N') && (str[pos + 1] == 'A') && (str[pos + 2] == 'N') && (str[pos + 3] >= '\0') &&
        ((unsigned char) str[pos + 3] <= ' ')) {
        if (Nchars != NULL) *Nchars = 3;
        return GSL_NAN;
    }

    // Read an optional leading sign
    if (str[pos] == '-') {
        negative = 1;
        pos++;
    } /* Deal with negatives */
    else if (str[pos] == '+') {
        pos++;
    } /* Deal with e.g. 1E+09 */

    if ((str[pos + 0] == '0') && (str[pos + 1] == 'x')) // Hexadecimal
    {
        pos += 2;
        // Accumulate hexadecimal digits; 48, 55 and 87 are the ASCII offsets of '0', 'A'-10 and 'a'-10
        while (1) {
            if ((str[pos] >= '0') && (str[pos] <= '9')) accumulator = ((16 * accumulator) + (((int) str[pos]) - 48));
            else if ((str[pos] >= 'A') && (str[pos] <= 'F'))
                accumulator = ((16 * accumulator) + (((int) str[pos]) - 55));
            else if ((str[pos] >= 'a') && (str[pos] <= 'f'))
                accumulator = ((16 * accumulator) + (((int) str[pos]) - 87));
            else break;
            pos++;
        }
        if (negative == 1) accumulator *= -1; /* Deals with negatives */
    } else {
        // Accumulate decimal digits as an integer, counting how many fall after the decimal point
        while (((str[pos] >= '0') && (str[pos] <= '9')) || (str[pos] == '.')) {
            // A decimal point switches on the counting of decimal places; 48 is the ASCII code of '0'
            if (str[pos] == '.') {
                past_decimal_point = 1;
            } else {
                accumulator = ((10 * accumulator) + (((int) str[pos]) - 48));
                if (past_decimal_point == 1) decimals++;
            }
            pos++;
        }

        while (decimals != 0) /* Deals with decimals */
        {
            decimals--;
            accumulator /= 10;
        }

        if (negative == 1) accumulator *= -1; /* Deals with negatives */

        // An exponent follows an 'e' or 'E', and is itself parsed recursively as a float
        if ((str[pos] == 'e') || (str[pos] == 'E'))
            accumulator *= pow(10.0, ppl_getFloat(str + pos + 1, &pos2)); /* Deals with exponents */

        if (pos2 > 0) pos += (1 + pos2); // Add on characters taken up by exponent, including one for the 'e' character.
    }
    if (pos == 0) pos = -1; // Alert the user that this was a blank string!
    if (Nchars != NULL) *Nchars = pos;
    return accumulator;
}

//! ppl_validFloat - See whether candidate string is a valid float
//! \param [in] str The input string
//! \param [out] end The number of characters occupied by the floating point value (optional)
//! \return Boolean flag indicating whether a valid floating-point value was found.

int ppl_validFloat(const char *str, int *end) {
    // Flags recording whether we have passed a decimal point, read any digits, and found a valid exponent
    unsigned char past_decimal_point = 0, had_number = 0, expvalid = 1;
    int pos = 0;
    int pos2 = 0;

    // The strings "nan" and "NAN", followed by whitespace or the end of the string, are valid floats
    if ((str[pos + 0] == 'n') && (str[pos + 1] == 'a') && (str[pos + 2] == 'n') && (str[pos + 3] >= '\0') &&
        ((unsigned char) str[pos + 3] <= ' ')) {
        pos += 3;
        had_number = 1;
        goto VALID_FLOAT_ENDED;
    }
    if ((str[pos + 0] == 'N') && (str[pos + 1] == 'A') && (str[pos + 2] == 'N') && (str[pos + 3] >= '\0') &&
        ((unsigned char) str[pos + 3] <= ' ')) {
        pos += 3;
        had_number = 1;
        goto VALID_FLOAT_ENDED;
    }

    // Skip over an optional leading sign
    if (str[pos] == '-') {
        pos++;
    } /* Deal with negatives */
    else if (str[pos] == '+') {
        pos++;
    } /* Deal with e.g. 1E+09 */

    if ((str[pos + 0] == '0') && (str[pos + 1] == 'x')) // Hexadecimal
    {
        pos += 2;
        // Skip over hexadecimal digits
        while (1) {
            if ((str[pos] >= '0') && (str[pos] <= '9')) pos++;
            else if ((str[pos] >= 'A') && (str[pos] <= 'F')) pos++;
            else if ((str[pos] >= 'a') && (str[pos] <= 'f')) pos++;
            else break;
            had_number = 1;
        }
        if (!had_number) return 0;
    } else {
        // Skip over decimal digits, allowing at most one decimal point
        while (((str[pos] >= '0') && (str[pos] <= '9')) || (str[pos] == '.')) {
            // A second decimal point ends the number
            if (str[pos] == '.') {
                if (past_decimal_point) goto VALID_FLOAT_ENDED;
                else past_decimal_point = 1;
            } else {
                had_number = 1;
            }
            pos++;
        }

        // A number must contain at least one digit; it may then be followed by an exponent
        if (!had_number) return 0;

        if ((str[pos] == 'e') || (str[pos] == 'E')) /* Deals with exponents */
        {
            expvalid = ppl_validFloat(str + pos + 1, &pos2);
            pos += pos2 + 1;
        }
    }

// The number has ended: it is valid if only whitespace follows it, or if the caller accepts trailing text. If *end is
// non-negative on entry, the number must occupy at least that many characters.
VALID_FLOAT_ENDED:
    while ((str[pos] != '\0') && ((unsigned char) str[pos] <= ' ')) pos++; /* Fast-forward over spaces at end */
    if ((!had_number) || (!expvalid)) return 0;
    if (end == NULL) return 1;
    if ((*end >= 0) && (pos < *end)) return 0;
    *end = pos;
    return 1;
}

//! ppl_numericDisplay - Render a string-representation of a double in either %f or %e formats
//! \param [in] in The floating point value to render
//! \param [out] output Character buffer into which to write the result (at least 128 bytes)
//! \param [in] SigFig The number of significant figures to display
//! \param [in] latex Boolean flag indicating whether we should produce LaTeX output (true) or human-readable output
//! (false)
//! \return Pointer to <output>

char *ppl_numericDisplay(const double in, char *output, const int SigFig, const int latex) {
    // A printf format string, built to show the required number of decimal places
    char format[16];
    double x, AccLevel;
    int decimal_level, i, j, k, l;
    // Moderately-sized numbers are shown in %f format, with the fewest decimal places which display the value to SigFig
    // significant figures
    if ((fabs(in) < 1e10) && (fabs(in) > 1e-3)) {
        x = fabs(in);
        AccLevel = x * (1.0 + pow(10, -SigFig));
        const int dp_max = (int) (SigFig - log10(x));
        for (decimal_level = 0; decimal_level < dp_max; decimal_level++)
            if ((x - ((floor(x * pow(10, decimal_level)) / pow(10, decimal_level)) - x)) < AccLevel) break;
        snprintf(format, 16, "%%.%df", decimal_level);
        snprintf(output, 128, format, in);
    } else {
        // Very large and very small numbers are shown in %e format, with the fewest decimal places which display the
        // mantissa to SigFig significant figures
        if (in == 0) {
            strcpy(output, "0");
        } else {
            x = fabs(in);
            x /= pow(10, (int) log10(x));
            AccLevel = x * (1.0 + pow(10, -SigFig));
            for (decimal_level = 0; decimal_level < SigFig; decimal_level++)
                if ((x - ((floor(x * pow(10, decimal_level)) / pow(10, decimal_level)) - x)) < AccLevel) break;
            snprintf(format, 16, "%%.%de", decimal_level);
            snprintf(output, 128, format, in);
            if (latex) // Turn 1e10 into nice latex
            {
                for (i = 0; ((output[i] != '\0') && (output[i] != 'e') && (output[i] != 'E')); i++);
                // Shift the exponent 32 characters to the right, to make room to rewrite "e" as "\times10^{"
                if (output[i] != '\0') {
                    for (j = i, k = i + 32; output[j] != '\0'; j++) output[j + 32] = output[j];
                    output[j + 32] = '\0';
                    // A mantissa of exactly 1 is omitted, giving 10^{n} rather than 1\times10^{n}
                    if ((i == 1) && (output[0] == '1')) {
                        strcpy(output, "10^{");
                        i = strlen(output); // Don't output 1 times 10^3
                    } else {
                        strcpy(output + i, "\\times10^{");
                        i += strlen(output + i); // Replace e with times ten to the...
                    }
                    k++; // FFW over the E
                    if (output[k] == '+') k++; // We don't need to say +8... 8 will do
                    // Copy the exponent's digits, dropping leading zeros; l is set once the first non-zero digit is
                    // reached
                    for (l = 0, j = k; output[j] != '\0'; j++) {
                        if ((output[j] > '0') && (output[j] <= '9')) l = 1;
                        if ((l == 1) || (output[j] != '0')) output[i++] = output[j];
                    } // Turn -08 into -8
                    output[i++] = '}';
                    output[i++] = '\0';
                }
            }
        }
    }
    for (i = 0; ((output[i] != '\0') && (output[i] != '.')); i++); // If we have trailing decimal zeros, get rid of them
    if (output[i] != '.') return output;
    for (j = i + 1; isdigit(output[j]); j++);
    if (i == j) return output;
    for (k = j - 1; output[k] == '0'; k--);
    if (k == i) k--;
    k++;
    if (k == j) return output;
    memmove(output + k, output + j, strlen(output + j) + 1);
    return output;
}

//! ppl_dblEqual - Test whether two floating-point values are approximately equal, to within one part in 10^7
//! \param [in] a First value
//! \param [in] b Second value
//! \return Boolean flag indicating equality

unsigned char ppl_dblEqual(const double a, const double b) {
    if ((fabs(a) < 1e-100) && (fabs(b) < 1e-100)) return 1;
    if ((fabs(a - b) <= fabs(1e-7 * a)) && (fabs(a - b) <= fabs(1e-7 * b))) return 1;
    return 0;
}

//! ppl_dblApprox - Test whether two floating-point values are equal to within an absolute tolerance
//! \param [in] a First value
//! \param [in] b Second value
//! \param [in] err The maximum absolute difference between <a> and <b>
//! \return Boolean flag indicating equality

unsigned char ppl_dblApprox(const double a, const double b, const double err) {
    if (fabs(a - b) > (fabs(err) + 1e-50)) return 0;
    return 1;
}

//! ppl_file_readline - Read a line from a file into a character buffer, up until the next newline character. The
//! output character buffer is always null-terminated. The file handle is fast-forwarded to the next new-line.
//! \param [in] file The file handle to read from
//! \param [in|out] output Pointer to the character buffer into which to write the characters read. If <MaxLenPtr> is
//! not NULL, this buffer must have been malloced, and it is realloced as required to hold the whole line.
//! \param [in|out] MaxLenPtr Pointer to the size of the malloced buffer <*output>, which is updated if the buffer
//! grows. If NULL, then a fixed-size buffer of size <MaxLength> is used, and long lines are truncated.
//! \param [in] MaxLength The maximum number of characters to write to output, including null termination. Only used
//! if <MaxLenPtr> is NULL.

void ppl_file_readline(FILE *file, char **output, int *MaxLenPtr, int MaxLength) {
    // The caller's buffer, used if it has a fixed size, and the number of characters read so far
    char *const fixedBuffer = *output;
    int i = 0;

    // A malloced buffer may grow beyond MaxLength, starting from its current size
    if (MaxLenPtr != NULL) MaxLength = *MaxLenPtr;

    // Read the file one character at a time with getc(), which is much faster than calling fscanf() per character
    while (!cancellationFlag) {
        const int ch = getc(file);
        const char c = (char) ch;
        if ((ch == EOF) || (c == '\n')) break;
        // Drop ASCII control characters other than tab (e.g. the \r of DOS line endings). Bytes above 127 are kept,
        // so that UTF-8 text survives.
        if ((ch < 32) && (c != '\t')) continue;
        // Store the character, growing or truncating as the type of buffer requires
        if (MaxLenPtr != NULL) {
            // Grow the malloced buffer as needed, leaving room for the null terminator
            if (i > MaxLength - 4) {
                char *const grown = realloc(*output, MaxLength * 2);
                if (grown == NULL) break;
                *output = grown;
                MaxLength *= 2;
            }
            (*output)[i++] = c;
        } else if (i < MaxLength - 2) {
            // Fixed-size buffer: truncate long lines, but keep reading to the end of the line
            fixedBuffer[i++] = c;
        }
    }
    // Null-terminate the line, and report the new size of a malloced buffer
    if (MaxLenPtr != NULL) {
        *MaxLenPtr = MaxLength;
        (*output)[i] = '\0';
    } else {
        fixedBuffer[i] = '\0';
    }
}

//! ppl_getWord - Returns the first word from <in>, terminated by any whitespace. Returns a maximum of <max> characters.
//! \param [out] out The character buffer into which to write the extracted word. Always null terminated.
//! \param [in] in The input character stream
//! \param [in] max The maximum number of characters to write to out, including null termination.

void ppl_getWord(char *out, const char *in, const int max) {
    int count = 0;
    while (((unsigned char) *in <= ' ') && (*in > '\0')) in++; /* Fast-forward over preceding whitespace */
    while ((((unsigned char) *in > ' ') || (*in < '\0')) && (count < (max - 1))) {
        *(out++) = *(in++);
        count++;
    }
    *out = '\0'; /* Terminate output */
}

//! ppl_nextWord - Fast forward a character pointer over one word, and return pointer to next word.
//! \param [in] in The pointer to the character array we are to advance by one word
//! \return The pointer to the first non-whitespace character of the second word in the input string

char *ppl_nextWord(char *in) {
    while (((unsigned char) *in <= ' ') && (*in > '\0')) in++; /* Fast-forward over preceding whitespace */
    while (((unsigned char) *in > ' ') || (*in < '\0')) in++; /* Fast-forward over one word */
    while (((unsigned char) *in <= ' ') && (*in > '\0')) in++; /* Fast-forward over whitespace before next word */
    return in; /* Return pointer to next word */
}

//! ppl_friendlyTimestring - Return pointer to time string in standard format, stored in a static string buffer which
//! will by overwritten by subsequent function calls. If the environment variable SOURCE_DATE_EPOCH is set, the time it
//! specifies is used in place of the current time, to allow reproducible builds.
//! \return A time string

char *ppl_friendlyTimestring() {
    time_t timenow;
    int override = 0;
    const char *source_date_epoch = getenv("SOURCE_DATE_EPOCH");
    // Use SOURCE_DATE_EPOCH, if it is set and is a valid integer
    if (source_date_epoch) {
        errno = 0;
        timenow = (time_t) strtol(source_date_epoch, NULL, 10);
        if (errno == 0) override = 1;
    }
    if (override == 0) timenow = time(NULL); // Get current time
    return (ctime(&timenow));
}

//! ppl_strStrip - Strip whitespace from both ends of a string and copy to a new character array
//! \param [in] in The input string to strip
//! \param [out] out Character buffer into which to write the stripped string
//! \return Pointer to <out>

char *ppl_strStrip(const char *in, char *out) {
    char *scan = out;
    while (((unsigned char) *in <= ' ') && (*in > '\0')) in++;
    while ((*in != '\0')) *(scan++) = *(in++);
    // Step back from the end of the copied string over any trailing whitespace, then terminate the string after the
    // last printable character
    scan--;
    while ((scan > out) && ((unsigned char) *scan <= ' ') && (*scan >= '\0')) scan--;
    *++scan = '\0';
    return out;
}

//! ppl_strUpper - Copy to capitalised version of a string into a new character buffer
//! \param [in] in The input string to be capitalised
//! \param [out] out The character buffer into which to write the capitalised version
//! \return Pointer to <out>

char *ppl_strUpper(const char *in, char *out) {
    char *scan = out;
    while (*in != '\0')
        // Shift ASCII lower-case letters into upper case; other bytes, including UTF-8 multi-byte characters, are
        // unchanged
        if ((*in >= 'a') && (*in <= 'z')) *scan++ = *in++ + 'A' - 'a';
        else *scan++ = *in++;
    *scan = '\0';
    return out;
}

//! ppl_strLower - Copy to lower-case version of a string into a new character buffer
//! \param [in] in The input string to be converted to lower-case
//! \param [out] out The character buffer into which to write the lower-case version
//! \return Pointer to <out>

char *ppl_strLower(const char *in, char *out) {
    char *scan = out;
    while (*in != '\0')
        // Shift ASCII upper-case letters into lower case; other bytes are unchanged
        if ((*in >= 'A') && (*in <= 'Z')) *scan++ = *in++ + 'a' - 'A';
        else *scan++ = *in++;
    *scan = '\0';
    return out;
}

//! ppl_strUnderline - Write a string of -s which is long enough to underline a string
//! \param [in] in The input string to be underlined
//! \param [out] out The character buffer into which to write the -s to underline <in>
//! \return Pointer to <out>

char *ppl_strUnderline(const char *in, char *out) {
    char *scan = out;
    // Write one dash for each printable character; control characters such as newlines get none
    while (*in != '\0') {
        if (((unsigned char) *in >= ' ') || (*in < '\0')) *scan++ = '-';
        in++;
    }
    *scan = '\0';
    return out;
}

//! ppl_strRemoveCompleteLine - Remove a single complete line from a text buffer, if there is one
//! \param [in|out] in The text buffer. If it contains a complete line, that line is removed from it.
//! \param [out] out Character buffer into which to write a stripped copy of the line removed
//! \return Pointer to <out>, which is an empty string if <in> did not contain a complete line

char *ppl_strRemoveCompleteLine(char *in, char *out) {
    char *scan2;
    char *scan = scan2 = in;
    char *scanout = out;
    while ((*scan != '\0') && (*scan != '\n')) scan++; // Find first carriage-return in text buffer
    if (*scan != '\0')
        while (scan2 < scan) *scanout++ = *scan2++; // If one is found, copy up to this point into temporary buffer
    *scanout = '\0';
    ppl_strStrip(out, out); // Strip it, and then this is what we will return

    if (*scan != '\0') // If we've taken a line out of the buffer, delete it from buffer
    {
        scan2 = in;
        while (*scan == '\n') scan++; // Forward over carriage return
        while (*scan != '\0') *scan2++ = *scan++;
        *scan2 = '\0';
    }
    return out;
}

//! ppl_strSlice - Take a slice out of a string and copy it into a new character buffer
//! \param [in] in The input string to take a slice from
//! \param [out] out A character buffer into which to write the slice
//! \param [in] start The first character to copy into a slice
//! \param [in] end The character after the last one to copy into the slice
//! \return Pointer to <out>

char *ppl_strSlice(const char *in, char *out, const int start, const int end) {
    char *scan = out;
    int pos = 0;
    while ((pos < start) && (in[pos] != '\0')) pos++;
    while ((pos < end) && (in[pos] != '\0')) *(scan++) = in[pos++];
    *scan = '\0';
    return out;
}

//! ppl_strCommaSeparatedListScan - Extract the next item from a comma-separated list, and copy it to <out>
//! \param [in|out] inscan A pointer to the input string. This is advanced to point to the character after the next
//! comma.
//! \param [out] out Character buffer into which to copy a stripped version of the next comma-separated value
//! \return Pointer to <out>

char *ppl_strCommaSeparatedListScan(char **inscan, char *out) {
    char *outscan = out;
    while ((**inscan != '\0') && (**inscan != ',')) *(outscan++) = *((*inscan)++);
    if (**inscan == ',') (*inscan)++; // Fast-forward over comma character
    *outscan = '\0';
    ppl_strStrip(out, out);
    return out;
}

//! ppl_strAutocomplete - Test whether a candidate string matches the beginning of test string, and is a least N
//! characters long
//! \param [in] candidate The string typed by the user, possibly abbreviated
//! \param [in] test The full string we are testing for an abbreviated match against
//! \param [in] Nmin The minimum number of characters which must be matched
//! \return The number of characters of <candidate> which were matched, or -1 if there was no match

int ppl_strAutocomplete(const char *candidate, const char *test, const int Nmin) {
    // i counts through <test>, and j through <candidate>
    int j;

    // Missing strings never match
    if ((candidate == NULL) || (test == NULL)) return -1;

    // Skip leading whitespace in the candidate
    for (j = 0; ((candidate[j] > '\0') && ((unsigned char) candidate[j] <= ' '));
         j++); // Fast-forward over whitespace at beginning

    // Compare the two strings character by character, case-insensitively, until the candidate stops matching
    for (int i = 0; 1; i++, j++)
        // Within the test string, a mismatch ends the comparison: the match is valid if enough characters matched and
        // the candidate's word has ended
        if (test[i] != '\0') {
            if (toupper(test[i]) != toupper(candidate[j])) // Candidate string has deviated from test string
            {
                if (i < Nmin) return -1; // We've not passed enough characters yet

                if ((unsigned char) candidate[j] <= ' ') // Word terminated with space...
                {
                    return j;
                } else if ((isalnum(candidate[j]) == 0) && (candidate[j] != '_')) // Word terminated with punctuation...
                {
                    // Alphanumeric test strings can be terminated by punctuation; others must have spaces after them
                    for (i = 0; test[i] != '\0'; i++)
                        if (((unsigned char) test[i] > ' ') && (isalnum(test[i]) == 0) && (test[i] != '_')) return -1;
                    return j;
                } else // Word terminated with more letters
                {
                    return -1;
                }
            }
        } else { // We've hit the end of the test string
            if ((unsigned char) candidate[j] <= ' ') // Word terminated with space...
            {
                return j;
            } else if ((isalnum(candidate[j]) == 0) && (candidate[j] != '_')) // Word terminated with punctuation...
            {
                // Alphanumeric test strings can be terminated by punctuation; others must have spaces after them
                for (i = 0; test[i] != '\0'; i++)
                    if (((unsigned char) test[i] > ' ') && (isalnum(test[i]) == 0) && (test[i] != '_')) return -1;
                return j;
            } else // Word terminated with more letters
            {
                return -1;
            }
        }
}

//! ppl_strWordWrap - Word wrap a piece of text to a certain width
//! \param [in] in The input text
//! \param [out] out Character buffer into which to write the word-wrapped text
//! \param [in] width The maximum line length

void ppl_strWordWrap(const char *in, char *out, const int width) {
    // State of the word-wrapping: whether we are in a run of whitespace, and the positions in <out> of the last space
    // and of the start of the current line
    int WhiteSpace = 1;
    int LastSpace = -1;
    int LineStart = 0;
    int LineFeeds =
            0; // If we meet > 1 linefeed during a period of whitespace, it marks the beginning of a new paragraph
    int i, j;
    // Copy the input to the output one character at a time; i counts through the input, and j through the output
    for (i = 0, j = 0; in[i] != '\0'; i++) {
        if ((WhiteSpace == 1) &&
            ((unsigned char) in[i] <= ' ')) // Once we've encountered one space, ignore any further whitespaceness
        {
            if (j == 0) j++; // If we open document with a new paragraph, we haven't already put down a space character
            // Two linefeeds in a period of whitespace means a new paragraph
            if ((in[i] == '\n') && (++LineFeeds == 2)) {
                // Two linefeeds in a period of whitespace means a new paragraph
                out[j - 1] = '\n';
                out[j] = '\n';
                LineStart = j++;
                LastSpace = -1;
            }
            continue;
        }
        if ((WhiteSpace == 0) && ((unsigned char) in[i] <= ' ')) // First whitespace character after a string of letters
        {
            if (in[i] == '\n') LineFeeds = 1;
            out[j] = ' ';
            LastSpace = j++;
            WhiteSpace = 1;
            continue;
        }
        // The markup used in the help text: "\\" forces a line break
        if ((in[i] == '\\') && (in[i + 1] == '\\')) {
            // Double-backslash implies a hard linebreak.
            i++;
            out[j] = '\n';
            LineStart = j++;
            LastSpace = -1;
            WhiteSpace = 1;
            continue;
        }
        // A hash character is a hard space, which is never broken
        if (in[i] == '#') {
            // A hash character implies a hard space character, used to tabulate data
            out[j++] = ' ';
            WhiteSpace = 1;
            continue;
        }
        WhiteSpace = 0;
        LineFeeds = 0;
        // A printable character: if the line is now too long, break it at the last space
        if (((j - LineStart) > width) && (LastSpace != -1)) {
            // If line is too long, insert a linebreak
            out[LastSpace] = '\n';
            LineStart = LastSpace;
            LastSpace = -1;
        }
        // The markup \lab and \rab stands for the angle brackets < and >
        if (strncmp(in + i, "\\lab", 4) == 0) {
            // Macros for left-angle-brackets, etc.
            i += 3;
            out[j++] = '<';
            continue;
        }
        // Right angle bracket
        if (strncmp(in + i, "\\rab", 4) == 0) {
            i += 3;
            out[j++] = '>';
            continue;
        }
        // The markup \VERSION and \DATE are replaced by Pyxplot's version number and release date
        if (strncmp(in + i, "\\VERSION", 8) == 0) {
            i += 7;
            strcpy(out + j, VERSION);
            j += strlen(out + j);
            continue;
        }
        // Release date
        if (strncmp(in + i, "\\DATE", 5) == 0) {
            i += 4;
            strcpy(out + j, DATE);
            j += strlen(out + j);
            continue;
        }
        // The word "Elephants" is the markup for a literal hash character, since # itself means a hard space
        if (strncmp(in + i, "Elephants", 9) == 0) {
            i += 8;
            out[j++] = '#';
            continue;
        }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

//! ppl_strBracketMatch - Find a closing bracket to match an opening bracket, and optionally return a list of all comma
//! positions
//! \param [in] in Pointer to the opening bracket character for which we are looking for the closing partner
//! \param [in] open The opening bracket character
//! \param [in] close The closing bracket character
//! \param [out] CommaPositions Array into which to write the positions of the opening bracket, each comma, and the
//! closing bracket (optional)
//! \param [out] Nargs The number of comma-separated arguments found, or -1 if no closing bracket was found (optional)
//! \param [out] ClosingBracketPos The position of the closing bracket, or -1 if none was found (optional)
//! \param [in] MaxCommaPoses The maximum number of entries to write to <CommaPositions>, or -1 for no limit

void ppl_strBracketMatch(const char *in, const char open, const char close, int *CommaPositions, int *Nargs,
                         int *ClosingBracketPos, const int MaxCommaPoses) {
    int BracketLevel[3] = {0, 0, 0};
    const char bopen[4] = "([{";
    const char bclose[4] = ")]}";
    int inpos = 0;
    int commapos = 0;
    char QuoteType = '\0';
    const int idx = (open == '{') ? 2 : ((open == '[') ? 1 : 0);
    int done = 0;

    // Scan through the string, keeping count of how deeply nested we are in each type of bracket
    for (; in[inpos] != '\0'; inpos++) {
        if (QuoteType != '\0') // Do not pay attention to brackets inside quoted strings
        {
            if ((in[inpos] == QuoteType) && (in[inpos - 1] != '\\')) QuoteType = '\0';
            continue;
        } else if ((in[inpos] == '\'') || (in[inpos] == '\"')) QuoteType = in[inpos]; // Entering a quoted string

        // Check whether this character opens or closes any of the three types of bracket. BracketLevel[idx] counts the
        // type we are matching; the other two must be at level zero for a match to count.
        for (int btype = 0; btype < 3; btype++) {
            if (in[inpos] == bopen[btype]) // Entering a nested level of brackets
            {
                BracketLevel[btype]++;
                if ((idx == btype) && (BracketLevel[btype] == 1) && (BracketLevel[(btype + 1) % 3] == 0) &&
                    (BracketLevel[(btype + 2) % 3] == 0) && (CommaPositions != NULL) &&
                    ((MaxCommaPoses < 0) || (commapos < MaxCommaPoses)))
                    *(CommaPositions + (commapos++)) = inpos; // Put ( on comma list
            } else if (in[inpos] == bclose[btype]) // Leaving a nested level of brackets
            {
                BracketLevel[btype]--;
                if ((idx == btype) && (BracketLevel[btype] == 0) && (BracketLevel[(btype + 1) % 3] == 0) &&
                    (BracketLevel[(btype + 2) % 3] == 0)) {
                    done = 1;
                    break;
                }
            }
        }

        // Commas at the top level of the bracket we are matching separate its arguments
        if ((in[inpos] == ',') && (BracketLevel[idx] == 1) && (BracketLevel[(idx + 1) % 3] == 0) &&
            (BracketLevel[(idx + 2) % 3] == 0)) // Found a new comma-separated item
        {
            if ((CommaPositions != NULL) && ((MaxCommaPoses < 0) || (commapos < MaxCommaPoses)))
                *(CommaPositions + (commapos++)) = inpos; // Put , on comma list
        }
        if (done) break;
    }

    // Report the closing bracket, or that none was found
    if (in[inpos] == '\0') {
        if (Nargs != NULL) *Nargs = -1;
        if (ClosingBracketPos != NULL) *ClosingBracketPos = -1;
    } else {
        if ((CommaPositions != NULL) && ((MaxCommaPoses < 0) || (commapos < MaxCommaPoses)))
            *(CommaPositions + (commapos++)) = inpos; // Put ) on comma list
        if (Nargs != NULL)
            *Nargs = commapos - 1; // There are N+1 arguments between N commas, but we've also counted ( and ).
        if (ClosingBracketPos != NULL) *ClosingBracketPos = inpos;
    }
}

//! ppl_strCmpNoCase - A case-insensitive version of the standard strcmp() function
//! \param [in] a First string
//! \param [in] b Second string
//! \return Comparison between strings

int ppl_strCmpNoCase(const char *a, const char *b) {
    char aU, bU;
    // Compare the strings one character at a time, until they differ or both end
    while (1) {
        if ((*a == '\0') && (*b == '\0')) return 0;
        // Identical characters: move on to the next pair
        if (*a == *b) {
            a++;
            b++;
            continue;
        }
        if ((*a >= 'a') && (*a <= 'z')) aU = *a - 'a' + 'A';
        else aU = *a;
        if ((*b >= 'a') && (*b <= 'z')) bU = *b - 'a' + 'A';
        else bU = *b;
        // Characters which match once converted to upper case: move on to the next pair
        if (aU == bU) {
            a++;
            b++;
            continue;
        }
        if (aU < bU) return -1;
        return 1;
    }
}

//! ppl_strEscapeSpaces - Copy a filename, putting a backslash before each space, so that wordexp() does not split it
//! into several words
//! \param [in] in - The filename
//! \param [out] out - The buffer into which to write the escaped filename, which is truncated if it does not fit
//! \param [in] outLen - The size of the buffer <out>

void ppl_strEscapeSpaces(const char *in, char *out, const size_t outLen) {
    size_t k = 0;
    for (int j = 0; (in[j] != '\0') && (k + 1 + (in[j] == ' ') < outLen); j++) {
        if (in[j] == ' ') out[k++] = '\\';
        out[k++] = in[j];
    }
    out[k] = '\0';
}

//! ppl_strEscapify - Copy a string into a new character buffer, enclosed in quotes and with escape characters inserted
//! before quote characters, backslashes, and control characters. If the buffer is too small, the escaped string is
//! truncated (but still closed with a quote, if there is room).
//! \param [in] in The input string
//! \param [out] out The character buffer into which to write the escaped string
//! \param [in] outLen The size of the buffer <out>
//! \return Pointer to <out>

char *ppl_strEscapify(const char *in, char *out, const size_t outLen) {
    // scanin points to the next character of the input; j is the number of bytes written to the output
    const char *scanin = in;
    size_t j = 0;
    // A buffer too small to hold even a pair of quotes receives an empty string
    if (outLen < 3) {
        if (outLen > 0) out[0] = '\0';
        return out;
    }
    out[j++] = '\"';
    // Leave room for the closing quote and the null terminator
    while ((*scanin != '\0') && (j + 2 < outLen - 1)) {
        char escape = '\0';
        // Characters which must be escaped, and the letters which follow the backslash in each case
        switch (*scanin) {
            case '\?':
                escape = '?';
                break;
            case '\a':
                escape = 'a';
                break;
            case '\b':
                escape = 'b';
                break;
            case '\f':
                escape = 'f';
                break;
            case '\n':
                escape = 'n';
                break;
            case '\r':
                escape = 'r';
                break;
            case '\t':
                escape = 't';
                break;
            case '\v':
                escape = 'v';
                break;
            case '\'':
            case '\"':
            case '\\':
                escape = *scanin;
                break;
            default:
                break;
        }
        // Write the character, preceded by a backslash if it needs escaping
        if (escape != '\0') {
            out[j++] = '\\';
            out[j++] = escape;
        } else {
            out[j++] = *scanin;
        }
        scanin++;
    }
    out[j++] = '\"';
    out[j] = '\0';
    return out;
}

//! ppl_strAppendf - Append printf-formatted text to a string in a fixed-size buffer, truncating it if the buffer is
//! full. The string always remains null-terminated, and <*pos> never passes the last byte of the buffer, so calls
//! may be chained safely.
//! \param [in,out] out The buffer
//! \param [in] outLen The size of the buffer <out>
//! \param [in,out] pos The position at which to append the text, which must be the end of the string in <out>. It
//! is advanced to the new end of the string.
//! \param [in] format The printf-style format string
//! \return The number of characters which would have been appended if the buffer were large enough

int ppl_strAppendf(char *out, const size_t outLen, int *pos, const char *format, ...) {
    va_list args;
    if ((*pos < 0) || ((size_t) *pos + 1 >= outLen)) return 0;
    va_start(args, format);
    const int n = vsnprintf(out + *pos, outLen - *pos, format, args);
    va_end(args);
    // vsnprintf() always terminates the string, but this guarantees it even if the buffer was filled exactly
    out[outLen - 1] = '\0';
    *pos += (int) strlen(out + *pos);
    return n;
}

//! ppl_strAppendStripped - Append a string to a string in a fixed-size buffer, with whitespace removed from both
//! ends, truncating it if the buffer is full
//! \param [in,out] out The buffer
//! \param [in] outLen The size of the buffer <out>
//! \param [in,out] pos The position at which to append the string, which is advanced past it
//! \param [in] in The string to append

void ppl_strAppendStripped(char *out, const size_t outLen, int *pos, const char *in) {
    const int start = *pos;
    ppl_strAppendf(out, outLen, pos, "%s", in);
    if (*pos <= start) return;
    ppl_strStrip(out + start,
                 out + start); // Stripping in place is safe, since the output never runs ahead of the input
    *pos = start + (int) strlen(out + start);
}

//! ppl_strAppendChar - Append a single character to a string in a fixed-size buffer, unless the buffer is full. The
//! string always remains null-terminated.
//! \param [in,out] out The buffer
//! \param [in] outLen The size of the buffer <out>
//! \param [in,out] pos The position at which to append the character, which is advanced past it
//! \param [in] c The character to append

void ppl_strAppendChar(char *out, const size_t outLen, int *pos, const char c) {
    if ((*pos < 0) || ((size_t) *pos + 1 >= outLen)) return;
    out[(*pos)++] = c;
    out[*pos] = '\0';
}

// Pyxplot's strings are stored as UTF-8, in which each character is encoded as one to four bytes. The first byte of
// each character is any byte except a continuation byte, which has the bit pattern 10xxxxxx. The following functions
// let strings be measured and indexed in characters rather than bytes.

//! utf8IsContinuation - Test whether a byte of a UTF-8 string continues a multi-byte character, rather than starting
//! a new character
//! \param [in] byte - The byte
//! \return True if <byte> is a continuation byte

static int utf8IsContinuation(const char byte) {
    return (((unsigned char) byte) & 0xC0) == 0x80;
}

//! ppl_utf8Length - Count the number of characters in a UTF-8 string
//! \param [in] in - The string
//! \return The number of characters in <in>

int ppl_utf8Length(const char *in) {
    int n = 0;
    for (int i = 0; in[i] != '\0'; i++)
        if (!utf8IsContinuation(in[i])) n++;
    return n;
}

//! ppl_utf8ByteOffset - Find the position, in bytes, of a character in a UTF-8 string
//! \param [in] in - The string
//! \param [in] charIndex - The number of the character, counting from zero
//! \return The byte offset of the start of the character, or the length of <in> in bytes if <charIndex> is beyond
//! the end of the string

int ppl_utf8ByteOffset(const char *in, const int charIndex) {
    int i, n = -1;
    for (i = 0; in[i] != '\0'; i++)
        // Each byte which starts a character advances the character count
        if (!utf8IsContinuation(in[i])) {
            n++;
            if (n == charIndex) return i;
        }
    return i;
}

//! ppl_utf8CharIndex - Find the number of the character in a UTF-8 string which begins at a given byte offset
//! \param [in] in - The string
//! \param [in] byteOffset - The byte offset
//! \return The number of characters which start before <byteOffset>

int ppl_utf8CharIndex(const char *in, const int byteOffset) {
    int n = 0;
    for (int i = 0; (i < byteOffset) && (in[i] != '\0'); i++)
        if (!utf8IsContinuation(in[i])) n++;
    return n;
}

//! ppl_utf8NextChar - Decode the next character of a UTF-8 string
//! \param [in] in - The string
//! \param [in,out] pos - The byte offset of the character to decode; advanced to the start of the next character
//! \return The Unicode code point of the character, or -1 if the bytes at <pos> are not valid UTF-8 (in which case
//! <pos> is advanced by one byte)

long ppl_utf8NextChar(const char *in, int *pos) {
    // s points to the first byte of the character; the number of bytes it occupies is given by the high bits of that
    // byte
    const unsigned char *s = (const unsigned char *) in + *pos;
    long codePoint;
    int nBytes;
    // The first byte gives the length of the character: 0xxxxxxx is ASCII, 110xxxxx starts two bytes, 1110xxxx three,
    // and 11110xxx four. Its remaining bits are the most significant bits of the code point.
    if (s[0] < 0x80) {
        nBytes = 1;
        codePoint = s[0];
    } else if ((s[0] & 0xE0) == 0xC0) {
        nBytes = 2;
        codePoint = s[0] & 0x1F;
    } else if ((s[0] & 0xF0) == 0xE0) {
        nBytes = 3;
        codePoint = s[0] & 0x0F;
    } else if ((s[0] & 0xF8) == 0xF0) {
        nBytes = 4;
        codePoint = s[0] & 0x07;
    } else {
        (*pos)++; // A continuation byte, or a byte which never appears in UTF-8
        return -1;
    }
    // Each continuation byte contributes six more bits of the code point
    for (int i = 1; i < nBytes; i++) {
        // Every byte after the first must be a continuation byte, 10xxxxxx
        if ((s[i] & 0xC0) != 0x80) {
            (*pos)++; // A multi-byte character which is cut short
            return -1;
        }
        codePoint = (codePoint << 6) | (s[i] & 0x3F);
    }
    *pos += nBytes;
    return codePoint;
}

//! utf8Locale - Fetch a locale which classifies Unicode characters (e.g. as letters), regardless of the user's own
//! locale setting
//! \return The locale, or (locale_t) 0 if no UTF-8 locale is available

static locale_t utf8Locale() {
    static int tried = 0;
    static locale_t locale = (locale_t) 0;
    // Look for a UTF-8 locale only once, since newlocale() is slow
    if (!tried) {
        tried = 1;
        locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t) 0);
        if (locale == (locale_t) 0) locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t) 0);
    }
    return locale;
}

//! ppl_utf8IsAlpha - Test whether a Unicode character is a letter, in any alphabet
//! \param [in] codePoint - The Unicode code point of the character, or -1 for an invalid character
//! \return True if the character is a letter

int ppl_utf8IsAlpha(const long codePoint) {
    locale_t locale;
    if (codePoint < 0) return 0;
    if (codePoint < 0x80) return isalpha((int) codePoint);
    locale = utf8Locale();
    if (locale == (locale_t) 0) return 0; // Without a UTF-8 locale, non-ASCII characters cannot be classified
    return iswalpha_l((wint_t) codePoint, locale);
}

//! ppl_utf8IsDigit - Test whether a Unicode character is one of the decimal digits 0-9
//! \param [in] codePoint - The Unicode code point of the character, or -1 for an invalid character
//! \return True if the character is a digit

int ppl_utf8IsDigit(const long codePoint) {
    return (codePoint >= '0') && (codePoint <= '9');
}

//! ppl_strWildcardTest - Test whether a string matches a wildcard pattern, in which ? matches any single character
//! and * matches any sequence of characters
//! \param [in] test The string to test
//! \param [in] wildcard The wildcard pattern
//! \return Boolean flag indicating whether <test> matches <wildcard>

int ppl_strWildcardTest(const char *test, const char *wildcard) {
    // i is the position reached in both strings; j is the end of the run of wildcard characters; mineat and maxeat are
    // the fewest and most characters which that run may match
    int i = 0, mineat = 0, maxeat = 0;

    // Match the literal characters before the first wildcard character
    while ((wildcard[i] != '\0') && (wildcard[i] != '?') && (wildcard[i] != '*')) {
        if (test[i] != wildcard[i]) return 0;
        i++;
    }
    if (wildcard[i] == '\0') return (test[i] == '\0');

    // Measure the run of wildcard characters which follows: each ? matches exactly one character, and any * makes the
    // number of characters unlimited
    int j = i;
    // Count the wildcard characters
    while ((wildcard[j] == '?') || (wildcard[j] == '*')) {
        // A ? consumes one character; a * consumes any number
        if (wildcard[j] == '?') {
            mineat++;
            maxeat++;
        } else {
            maxeat = 10000;
        }
        j++;
    }

    // Consume the characters which the run must match
    for (int k = 0; k < mineat; k++)
        if (test[i++] == '\0') return 0;

    // Try matching the rest of the pattern after each possible number of further characters, recursively
    for (int k = 0; k < maxeat - mineat; k++) {
        if (ppl_strWildcardTest(test + i, wildcard + j)) return 1;
        if (test[i++] == '\0') return 0;
    }
    return 0;
}

//! ppl_snprintfTrunc - Write formatted output into a fixed-size buffer, truncating it if it does not fit. This is
//! intended for messages, where truncation is acceptable; it must not be used for filenames or commands, where
//! truncation would change their meaning.
//! \param [out] out - The character buffer into which to write the output. It is always null-terminated.
//! \param [in] size - The size of the buffer <out>
//! \param [in] format - The printf-style format string
//! \return The number of characters which would have been written had <out> been large enough, as for snprintf()

int ppl_snprintfTrunc(char *out, const size_t size, const char *format, ...) {
    va_list args;
    if (size == 0) return 0;
    va_start(args, format);
    const int n = vsnprintf(out, size, format, args);
    va_end(args);
    out[size - 1] = '\0';
    return n;
}
