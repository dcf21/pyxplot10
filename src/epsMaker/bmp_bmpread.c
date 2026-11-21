// bmp_bmpread.c
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
// Foundation; either version 2 of the License, -1, -1, or (at your option) any later
// version.
//
// You should have received a copy of the GNU General Public License along with
// Pyxplot; if not, write to the Free Software Foundation, Inc., 51 Franklin
// Street, Fifth Floor, Boston, MA  02110-1301, USA

// ----------------------------------------------------------------------------

// Reads Windows and OS/2 bitmap (BMP) image files for the image command (eps_image.c), decoding them into the
// bitmap_data structure used by Pyxplot's image code. Paletted images of 1, 4 and 8 bits per pixel, including
// run-length encoded ones, are supported, as are 16-bit and 24-bit RGB images, which are converted to 24-bit RGB. Rows
// are stored bottom-up in BMP files, and are flipped as they are read. Edited from code kindly contributed to Pyxplot
// by Michael Rutter.

#define _PPL_BMP_BMPREAD_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "coreUtils/memAlloc.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/errorReport.h"

#include "epsMaker/bmp_image.h"
#include "epsMaker/bmp_bmpread.h"

//! ppl_bmp_bmpread - Read a Windows bitmap (BMP) image file. On failure, an error is reported and <image->data> is set
//! to NULL.
//! \param [in] ec - The error-reporting context
//! \param [in] in - The file handle of the BMP image
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_bmpread(pplerr_context *ec, FILE *in, bitmap_data *image) {
    // Variables describing the image; off2 counts the bytes read so far
    char msg[PPL_MSG_LENGTH];
    unsigned char buff[60], encode, *p, c;
    unsigned width, height, depth, dw, excess;
    unsigned long offset, off2, size;
    int i, j, ncols, os2, rle;

    // Log progress in debugging mode
    if (DEBUG) ppl_log(ec, "Beginning to decode BMP image file");

    // The first three bytes of the file have already been read to identify it
    off2 = 3;

    // Read rest of first header
    if (fread(buff, 11, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
        return;
    }
    off2 += 11;

    // Four reserved bytes must be zero
    if ((buff[3]) || (buff[4]) || (buff[5]) || (buff[6])) {
        ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
        return;
    }

    // The offset of the pixel data from the start of the file
    offset = buff[7] + (buff[8] << 8) + (buff[9] << 16) + (buff[10] << 24);

    // Read info header
    if (fread(buff, 12, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
        return;
    }
    off2 += 12;

    if ((buff[0] == 12) && (buff[1] == 0)) // OS/2 1.x bitmap
    {
        os2 = 1;
        // There must be one color plane
        if ((buff[8] != 1) || (buff[9])) {
            ppl_error(ec, ERR_FILE, -1, -1, "This OS/2 bitmap file appears to be corrupted");
            return;
        }
        width = buff[4] + ((unsigned) buff[5] << 8);
        height = buff[6] + ((unsigned) buff[7] << 8);
        depth = buff[10];
        encode = 0;
        size = 0;
    } else // Windows bitmap
    {
        os2 = 0;

        // Known valid header lengths at this point are 40, 64, 108 and 124
        // So accept from 40 to 255
        if ((buff[0] < 40) || (buff[1]) || (buff[2]) || (buff[3])) {
            ppl_error(ec, ERR_FILE, -1, -1, "This Windows bitmap file appears to be corrupted");
            return;
        }
        excess = buff[0] - 40;

        // Read the rest of the 40-byte header
        if (fread(buff + 12, 40 - 12, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
            return;
        }
        off2 += 40 - 12;

        // Skip any longer header
        if (excess) {
            off2 += excess;
            for (; excess; excess--) fgetc(in); // Can't seek stdin
        }

        // Read the size of the image
        width = buff[4] + ((unsigned) buff[5] << 8) + ((unsigned) buff[6] << 16) + ((unsigned) buff[7] << 24);
        height = buff[8] + ((unsigned) buff[9] << 8) + ((unsigned) buff[10] << 16) + ((unsigned) buff[11] << 24);

        // There must be one color plane
        if ((buff[12] != 1) || (buff[13]) || (buff[15])) {
            ppl_error(ec, ERR_FILE, -1, -1, "This Windows bitmap file appears to be corrupted");
            return;
        }

        // The depth, compression type and size of the pixel data
        depth = buff[14];
        encode = buff[16];
        size = buff[20] + (((int) (buff[21])) << 8) + (((int) (buff[22])) << 16) + (((int) (buff[23])) << 24);

        // The resolution, converted from pixels per metre to dots per inch
        image->XDPI = 0.0254 *
                (buff[24] + (((int) (buff[25])) << 8) + (((int) (buff[26])) << 16) + (((int) (buff[27])) << 24));
        image->YDPI = 0.0254 *
                (buff[28] + (((int) (buff[29])) << 8) + (((int) (buff[30])) << 16) + (((int) (buff[31])) << 24));
        if (image->XDPI < 1) image->XDPI = 180; // Sensible default resolution in case of zero input
        if (image->YDPI < 1) image->YDPI = 180;
    }

    // Log the size and depth in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Size %dx%d depth %d bits", width, height, depth);
        ppl_log(ec, msg);
    }

    // Only RLE8 for 8-bit images, RLE4 for 4-bit images, and bit fields for 16-bit images are supported
    rle = 0;
    // Check the compression type
    if (encode != 0) {
        if ((encode == 1) && (depth == 8)) rle = 8;
        else if ((encode == 2) && (depth == 4)) rle = 4;
        // Bit fields must be 16-bit
        else if ((encode != 3) || (depth != 16)) {
            ppl_error(ec, ERR_FILE, -1, -1, "This Windows bitmap file has an invalid encoding");
            return;
        }
    }

    // Check the depth is supported
    if ((depth != 1) && (depth != 4) && (depth != 8) && (depth != 16) && (depth != 24)) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Bitmap colour depth of %d not supported\n", depth);
        ppl_error(ec, ERR_FILE, -1, -1, msg);
        return;
    }

    if (depth <= 8) // We have a palette to read
    {
        ncols = 0;
        if (!os2) ncols = buff[32] + (buff[33] << 8) + (buff[34] << 16) + (buff[35] << 24);
        if (!ncols) ncols = 1 << depth;

        // The palette cannot be longer than the depth allows
        if (ncols > (1 << depth)) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Bitmap image has a palette length of %d, which is not possible with a colour depth of %d", ncols,
                    depth);
            ppl_error(ec, ERR_FILE, -1, -1, msg);
            return;
        }

        // Allocate the palette
        image->pal_len = ncols;
        image->palette = ppl_memAlloc(3 * image->pal_len);
        off2 += (4 - os2) * image->pal_len;
        // Failure to allocate memory is reported as an error
        if (image->palette == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return;
        }
        p = image->palette;
        // Read the palette one entry at a time, converting BGR to RGB
        for (i = 0; i < image->pal_len; i++) {
            // MS Windows uses BGR0, OS/2 uses BGR
            if (fread(buff, 4 - os2, 1, in) != 1) {
                ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
                return;
            }
            *(p++) = buff[2];
            *(p++) = buff[1];
            *(p++) = buff[0];
        }
        image->type = BMP_COLOUR_PALETTE;
        image->colour = BMP_COLOUR_PALETTE;
    } else {
        image->type = BMP_COLOUR_BMP;
        image->colour = BMP_COLOUR_RGB;
    }

    if ((depth == 16) && (encode == 3)) // Must read 12 byte pseudopalette
    {
        // Read the bit masks
        if (fread(buff + 40, 12, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
            return;
        }
        off2 += 12;
    }

    // Skip to the pixel data
    if (offset < off2) {
        ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
        return;
    } else if (offset > off2) {
        // Log the extra data in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "%ld bytes of extra data", offset - off2);
            ppl_log(ec, msg);
        }
        for (; off2 < offset; off2++) fgetc(in);
    }

    // Record the size of the image
    image->height = height;
    image->width = width;

    // The number of bytes per row
    dw = (width * depth + 7) / 8;
    image->data_len = dw * height;
    image->depth = depth;

    // 16-bit images are converted to 24-bit RGB
    if (depth == 16) {
        ppl_bmp_bmp16read(ec, in, buff, image);
        return;
    }

    // Uncompressed images are read row by row; run-length encoded ones are decoded
    if (!rle) {
        image->data = (unsigned char *) ppl_memAlloc(image->data_len);
        // Failure to allocate memory is reported as an error
        if (image->data == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return;
        }
        // Read each row, from the bottom of the image up
        for (i = 1; i <= height; i++) {
            // Read the row
            if (fread(image->data + (height - i) * dw, dw, 1, in) != 1) {
                ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
                return;
            }
            // Skip the padding at the end of the row
            if (dw & 3) {
                // Read the padding
                if (fread(buff, 4 - (dw & 3), 1, in) != 1) {
                    ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
                    return;
                }
            } // Lines are dword aligned
        }

        if (depth == 24) // BMP is BGR not RGB
        {
            p = image->data;
            // Swap the red and blue components of each pixel
            for (i = 0; i < height; i++) {
                // Each pixel in the row
                for (j = 0; j < width; j++) {
                    c = *p;
                    *p = *(p + 2);
                    *(p + 2) = c;
                    p += 3;
                }
            }
        }
    } else { // if (rle)
        p = malloc(size);
        // Failure to allocate memory is reported as an error
        if (p == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            image->data = NULL;
            return;
        }
        // Read the encoded data
        if (fread(p, size, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
            free(p);
            p = NULL;
            return;
        }
        // Decode it
        if (ppl_bmp_demsrle(ec, image, p, size) != 0) {
            image->data = NULL;
            free(p);
            p = NULL;
            return;
        }
        free(p);
        p = NULL;
    }
    return;
}

//! ppl_bmp_bmp16read - Read the pixel data of a 16-bit-per-pixel (15- or 16-bit RGB) Windows bitmap image, converting
//! it to 24-bit RGB
//! \param [in] ec - The error-reporting context
//! \param [in] in - The file handle of the BMP image, positioned at the start of the pixel data
//! \param [in] header - The bitmap file's header, from which the bit masks are read
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_bmp16read(pplerr_context *ec, FILE *in, unsigned char *header, bitmap_data *image) {
    // Variables for decoding each pixel
    unsigned char *palette;
    unsigned char *rowptr, *ptr;
    unsigned int width, height, colour;
    int is15, i, j;

    // Assume 5 bits per component unless the bit masks say otherwise
    is15 = 1;
    width = image->width;
    height = image->height;

    if (header[16] == 3) // Read "palette" and check correct
    { // Palette can be 7c00 03e0 001f (15 bit)
        // or             f800 07e0 001f (16 bit)
        // as represents RGB bitmasks. However,
        // Intel byte ordering afflicts the above,
        // and the entries are stored as dwords,
        // not words.
        palette = header + 40;
        // Check the bit masks
        if ((palette[0] != 0) || (palette[4] != 0xe0) || (palette[8] != 0x1f) || (palette[9] != 0)) {
            ppl_error(ec, ERR_FILE, -1, -1, "This 16-bit bitmap file appears to be corrupted");
            return;
        }

        // A red mask of f800 means 5-6-5 bits
        if (palette[1] & 0x80) is15 = 0;
    } else if (header[30] != 0) {
        ppl_error(ec, ERR_FILE, -1, -1, "This 16-bit bitmap file has invalid compression type");
        return;
    }

    // Log the format in debugging mode
    if (DEBUG) {
        if (is15) ppl_log(ec, "15 bit 555");
        else ppl_log(ec, "16 bit 565");
    }

    // The output is 24-bit RGB
    image->depth = 24;
    image->data_len = 3 * width * height;
    image->data = (unsigned char *) ppl_memAlloc(image->data_len);
    // Failure to allocate memory is reported as an error
    if (image->data == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return;
    }

    // A buffer for one row of input
    rowptr = malloc(2 * width);
    // Failure to allocate memory is reported as an error
    if (rowptr == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        image->data = NULL;
        return;
    }

    // Read each row, from the bottom of the image up
    for (i = 1; i <= height; i++) {
        // Read the row
        if (fread(rowptr, 2 * width, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
            free(rowptr);
            rowptr = NULL;
            return;
        }
        ptr = image->data + (height - i) * 3 * width;
        // Expand each pixel's components to 8 bits
        for (j = 0; j < width; j++) {
            colour = rowptr[2 * j] + (rowptr[2 * j + 1] << 8);
            *(ptr + 2) = (colour & 0x1f) << 3; // blue
            if (is15) // green
            {
                *(ptr + 1) = (colour & 0x03e0) >> 2;
            } else {
                *(ptr + 1) = (colour & 0x07e0) >> 3;
                colour >>= 1;
            }
            *ptr = (colour & 0x7c00) >> 7; // red
            ptr += 3;
        }
        // Skip the padding at the end of the row
        if (width & 1) {
            // Read the padding
            if (fread(rowptr, 2, 1, in) != 1) {
                ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file appears to be corrupted");
                free(rowptr);
                rowptr = NULL;
                return;
            }
        } // rows are dword aligned
    }
    free(rowptr);
    rowptr = NULL;
    return;
}

//! ppl_bmp_demsrle - Decode the run-length encoded pixel data of a 4- or 8-bit Windows bitmap image
//! \param [in] ec - The error-reporting context
//! \param [in] image - The image structure into which the image is read
//! \param [in] in - The run-length encoded data
//! \param [in] len - The number of bytes of encoded data
//! \return Zero on success, or one if the data was corrupt or memory allocation failed

int ppl_bmp_demsrle(pplerr_context *ec, bitmap_data *image, unsigned char *in, const unsigned long len) {
    // Variables for decoding
    char msg[PPL_MSG_LENGTH];
    unsigned char *out, *c_in, *c_out, code, *end, odd, even, *inEnd;
    unsigned long i, j, delta, size, height, width;
    int eol, rle;

    // The decoded image has one byte per pixel
    height = image->height;
    width = image->width;
    rle = image->depth;
    // Only 4-bit and 8-bit images can be run-length encoded
    if ((rle != 4) && (rle != 8)) {
        ppl_error(ec, ERR_FILE, -1, -1, "This bitmap file has an impossible MSRLE image depth");
        return 1;
    }
    // Log the encoding in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Bitmap image has RLE%d compression\n", rle);
        ppl_log(ec, msg);
    }

    // The output has one byte per pixel
    image->depth = 8;
    size = height * width;
    image->data_len = size;

    // Allocate the output
    out = (unsigned char *) ppl_memAlloc(image->data_len);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return 1;
    }

    // Pixels skipped over by delta codes are left as colour zero
    memset(out, 0, image->data_len);

    // Pointers into the input and output
    c_in = in;
    c_out = out;
    inEnd = in + len;

    // Decode each row, from the bottom of the image up. Rows are stored from the top down, so row i starts at
    // out + (height - i) * width.
    for (i = 1; i <= height; i++) {
        c_out = out + (height - i) * width;
        end = c_out + width - 1;
        eol = 0;
        // Decode codes until an end-of-line or end-of-data code. Pixels beyond the end of the row, such as the padding
        // which some encoders add to make rows a multiple of four pixels long, are discarded, and pixels which are not
        // written, because the row ends early, are left as colour zero.
        while (eol == 0) {
            // Each code is at least two bytes long; data which simply stops after the last row is complete is accepted
            if (c_in + 2 > inEnd) {
                if ((i >= height) && (c_out > end)) {
                    eol = 2;
                    break;
                }
                goto truncated;
            }
            code = *c_in++;
            if (code) // a run
            {
                if (rle == 8) odd = even = *c_in++;
                // In RLE4, a run alternates between two pixel values
                else {
                    odd = *c_in & 0xf;
                    even = (*c_in & 0xf0) >> 4;
                    c_in++;
                }
                // Write the run, without passing the end of the row
                for (j = 0; (j < code) && (c_out <= end); j++) *c_out++ = (j & 1) ? odd : even;
            } else {
                code = *c_in++;
                // A zero byte is followed by an escape code: end of line, end of data, a jump, or a run of literal
                // pixels
                switch (code) {
                    case (0): // EOL
                        eol = 1;
                        break;
                    case (1): // EOD
                        eol = 2;
                        break;
                    case (2): // Delta
                    {
                        // Jump dx pixels right and dy rows up the image, i.e. towards the start of the output buffer
                        unsigned long column = c_out - (out + (height - i) * width);
                        if (c_in + 2 > inEnd) goto truncated;
                        delta = *c_in++; // dx
                        column += delta;
                        delta = *c_in++; // dy
                        // The jump must stay within the image
                        if ((i + delta > height) || (column > width)) {
                            ppl_error(ec, ERR_FILE, -1, -1, "Image overflow whilst decoding RLE in bitmap image file");
                            return 1;
                        }
                        i += delta;
                        c_out = out + (height - i) * width + column;
                        end = out + (height - i) * width + width - 1;
                        break;
                    }
                    default: // Literal
                    {
                        // The literal pixels take one byte each in RLE8, or two per byte in RLE4, padded to a whole
                        // number of 16-bit words
                        const unsigned long nbytes = (rle == 8) ? code : ((code + 1) / 2);
                        if (c_in + nbytes + (nbytes & 1) > inEnd) goto truncated;
                        // Copy the literal pixels, without passing the end of the row
                        for (j = 0; (j < code) && (c_out <= end); j++) {
                            if (rle == 8) *c_out++ = c_in[j];
                            else *c_out++ = (j & 1) ? (c_in[j / 2] & 0xf) : ((c_in[j / 2] & 0xf0) >> 4);
                        }
                        c_in += nbytes + (nbytes & 1); // Keep word aligned
                    }
                }
            }
        }
        // An end-of-data code leaves the remaining rows as colour zero
        if (eol == 2) break;
    }

    // Return the decoded image
    image->data = out;
    return 0;

// The encoded data ended before the image was complete
truncated:
    ppl_error(ec, ERR_FILE, -1, -1, "Bitmap image file ended unexpectedly whilst decoding RLE");
    return 1;
}
