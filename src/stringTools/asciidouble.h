// asciidouble.h
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

#ifndef ASCIIDOUBLE_H
#define ASCIIDOUBLE_H 1

#include <stdio.h>

double ppl_getFloat(const char *str, int *Nchars);

int ppl_validFloat(const char *str, int *end);

char *ppl_numericDisplay(double in, char *output, int SigFig, int latex);

unsigned char ppl_dblEqual(double a, double b);

unsigned char ppl_dblApprox(double a, double b, double err);

void ppl_file_readline(FILE *file, char **output, int *MaxLenPtr, int MaxLength);

void ppl_getWord(char *out, const char *in, int max);

char *ppl_nextWord(char *in);

char *ppl_friendlyTimestring();

char *ppl_strStrip(const char *in, char *out);

char *ppl_strUpper(const char *in, char *out);

char *ppl_strLower(const char *in, char *out);

char *ppl_strUnderline(const char *in, char *out);

char *ppl_strRemoveCompleteLine(char *in, char *out);

char *ppl_strSlice(const char *in, char *out, int start, int end);

char *ppl_strCommaSeparatedListScan(char **inscan, char *out);

int ppl_strAutocomplete(const char *candidate, const char *test, int Nmin);

void ppl_strWordWrap(const char *in, char *out, int width);

void ppl_strBracketMatch(const char *in, char open, char close, int *CommaPositions, int *Nargs, int *ClosingBracketPos,
                         int MaxCommaPoses);

int ppl_strCmpNoCase(const char *a, const char *b);

char *ppl_strEscapify(const char *in, char *out, size_t outLen);
int ppl_strAppendf(char *out, size_t outLen, int *pos, const char *format, ...) __attribute__((format(printf, 4, 5)));
void ppl_strAppendChar(char *out, size_t outLen, int *pos, char c);
void ppl_strAppendStripped(char *out, size_t outLen, int *pos, const char *in);

int ppl_strWildcardTest(const char *test, const char *wildcard);

int ppl_utf8Length(const char *in);
int ppl_utf8ByteOffset(const char *in, int charIndex);
int ppl_utf8CharIndex(const char *in, int byteOffset);
long ppl_utf8NextChar(const char *in, int *pos);
int ppl_utf8IsAlpha(long codePoint);
int ppl_utf8IsDigit(long codePoint);

int ppl_snprintfTrunc(char *out, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));

#endif
