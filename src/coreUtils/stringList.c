// stringList.c
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

// Splits strings into lists of words. It is used by the help command to split help topics into words.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stringTools/asciidouble.h"

#include "list.h"

//! ppl_strSplit - Split up a string into words separated by whitespace
//! \param [in] in - The input string to split
//! \return A list of the words in <in>, each stored as a null-terminated string. The list is allocated in the
//! current ppl_memAlloc context.

list *ppl_strSplit(char *in) {
    list *out = ppl_listInit(0);
    int pos = 0;
    char *text_buffer = (char *) malloc(strlen(in) + 1);
    // Find each word in turn
    while (in[pos] != '\0') {
        // Scan along to find the next word
        while (((unsigned char) in[pos] <= ' ') && (in[pos] > '\0')) pos++;
        const int start = pos;

        // Scan along to find the end of this word
        while (((unsigned char) in[pos] > ' ') && (in[pos] > '\0')) pos++;
        const int end = pos;

        // Copy each non-empty word into the list
        if (end > start) {
            char *word = ppl_strSlice(in, text_buffer, start, end);
            ppl_listAppendCpy(out, word, strlen(word) + 1);
        }
    }
    free(text_buffer);
    text_buffer = NULL;
    return out;
}
