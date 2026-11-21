// bmp_a85.c
//
// The code in this file is part of Pyxplot
// <http://www.pyxplot.org.uk>
//
// Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
//               2008-2013 Ross Church
//
//               2009-2010 Michael Rutter
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

// Encodes binary data, such as bitmap images and their palettes, in postscript's ASCII85 format, which uses all of the
// printable ASCII characters, so that it can be included in the text of a postscript file. Used by eps_image.c and by
// the colormap plot style. Edited from code kindly contributed to Pyxplot by Michael Rutter.

#define _PPL_BMP_A85_C 1

#include <stdlib.h>
#include <stdio.h>
#include "coreUtils/errorReport.h"

// Number of raw bytes to encode per line. Note actual line length will be
// 1.25x this. Sane values are 56 (70 characters) or 60 (75 chars). Must be a
// multiple of 4.

#define LINELEN 56

//! ppl_bmp_A85 - Write binary data to a file using ASCII85 encoding, as used in postscript, encoding LINELEN bytes of
//! input on each line of output
//! \param [in] ec - The error-reporting context
//! \param [in] fout - The file handle to write to
//! \param [in] in - The binary data to encode
//! \param [in] len - The number of bytes of data to encode
//! \return The number of characters written

unsigned int ppl_bmp_A85(const pplerr_context *ec, FILE *fout, const unsigned char *in, int len) {
    // Each group of four bytes is written as five base-85 digits, offset by '!'
    int i, j;
    unsigned int tmp, t, line, length = 0;
    unsigned int pow85[] = {52200625, 614125, 7225, 85, 1}; // powers of 85
    unsigned char out[LINELEN / 4 * 5 + 4], *outp;

    // Encode the data one line at a time
    while (len > 0) {
        line = (len > LINELEN) ? LINELEN : len;
        len = len - line;
        outp = out;
        for (i = 0; i < (line & 0xfffc); i += 4) // encode groups of four bytes
        {
            tmp = 0;
            // Make a 32-bit number from four bytes
            for (j = 0; j < 4; j++) {
                tmp = tmp << 8;
                tmp += *(in++);
            }
            // Write the five base-85 digits, most significant first
            if (tmp) {
                // Each of the first four digits
                for (j = 0; j < 4; j++) {
                    t = tmp / pow85[j];
                    tmp -= t * pow85[j];
                    *(outp++) = t + '!';
                }
                *(outp++) = tmp + '!';
            } else *(outp++) = 'z'; // zero is encoded as "z", not "!"
        }

        if (line & 0x3) // Deal with any final group of <4 bytes
        {
            i = line & 0xfffc;
            tmp = 0;
            // Make a 32-bit number from the remaining bytes, padded with zeros
            for (j = 0; j < 4; j++) {
                tmp = tmp << 8;
                if ((i + j) < line) tmp += *(in++);
            }
            // A group of n bytes is written as n+1 digits
            for (j = 0; j < ((line & 0x3) + 1); j++) {
                t = tmp / pow85[j];
                tmp -= t * pow85[j];
                *(outp++) = t + '!';
            }
        }

        *outp = 0;

        // Assist things which parse DSC comments by ensuring that any line which
        // would start %! or %% has a space prefixed.

        // Write the line, with a leading space if it could be mistaken for a postscript comment
        if ((out[0] == '%') && ((out[1] == '%') || (out[1] == '!'))) length += fprintf(fout, " %s\n", out);
        else length += fprintf(fout, "%s\n", out);
    }

    // Mark the end of the data
    length += fprintf(fout, "~>\n");
    return length;
}
