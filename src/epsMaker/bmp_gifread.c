// bmp_gifread.c
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

// Reads GIF image files for the image command (eps_image.c), decoding them into the bitmap_data structure used by
// Pyxplot's image code. The header, color maps and extension blocks are parsed (the graphic control extension may give
// a transparent color), the LZW-compressed pixel data are decompressed, and interlaced images are reordered. Only the
// first image in a file is read, and it must fill the whole of the logical screen. Edited from code kindly contributed
// to Pyxplot by Michael Rutter.

#define _PPL_BMP_GIFREAD_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "coreUtils/memAlloc.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/errorReport.h"

#include "epsMaker/bmp_image.h"
#include "epsMaker/bmp_gifread.h"

//! ppl_bmp_gifread - Read a GIF image file. On failure, an error is reported and <image->data> is set to NULL.
//! \param [in] ec - The error-reporting context
//! \param [in] in - The file handle of the GIF image
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_gifread(pplerr_context *ec, FILE *in, bitmap_data *image) {
    // Variables describing the image
    char msg[PPL_MSG_LENGTH];
    unsigned char buff[8], flags, len, *rawz;
    int gcm, ncols, interlaced;
    long lxoff, lyoff, lw, lh, lzwcs;
    unsigned long datalen, width, height;
    static unsigned char trans;

    // Log progress in debugging mode
    if (DEBUG) ppl_log(ec, "Beginning to decode GIF image file");

    // Read the version, after the letters GIF which have already been read
    if (fread(buff, 3, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }
    buff[3] = 0;

    // Only versions 87a and 89a are recognised
    if (!strcmp((char *) buff, "87a")) {
        if (DEBUG) ppl_log(ec, "GIF image in format GIF87a");
    } else if (!strcmp((char *) buff, "89a")) {
        if (DEBUG) ppl_log(ec, "GIF image in format GIF89a");
    } else {
        ppl_error(ec, ERR_FILE, -1, -1, "GIF image file is in an unrecognised format");
        return;
    }

    // Read the size of the logical screen
    if (fread(buff, 4, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }
    width = ((unsigned) buff[1] << 8) + buff[0];
    height = ((unsigned) buff[3] << 8) + buff[2];

    // Log the size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Size %ldx%ld", width, height);
        ppl_log(ec, msg);
    }

    // Read the flags describing the global color map
    if (fread(&flags, 1, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }

    // Log whether there is a global color map in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Global colour map=%d", (flags & 0x80) >> 7);
        ppl_log(ec, msg);
    }
    // Log the color resolution in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Colour resolution=%d", (flags & 0x70) >> 4);
        ppl_log(ec, msg);
    }
    // Log the color depth in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Colour depth     =%d", (flags & 0x7));
        ppl_log(ec, msg);
    }

    // Whether there is a global color map, and its length
    gcm = ((flags & 0x80) >> 7);
    ncols = 2 << (flags & 0x7);

    // Log the number of colors in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Global number of colours=%d", ncols);
        ppl_log(ec, msg);
    }

    // Skip the background color and aspect ratio
    if (fread(buff, 2, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }

    // Read the global color map
    if (gcm) {
        image->palette = (unsigned char *) ppl_memAlloc(ncols * 3);
        // Failure to allocate memory is reported as an error
        if (image->palette == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return;
        }
        // Read the colors
        if (fread(image->palette, ncols * 3, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
            return;
        }
        image->pal_len = ncols;
    }

    // Read the type of the next block
    if (fread(&flags, 1, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }

    // Process each extension block
    while (flags == 0x21) {
        // Read the type of the extension
        if (fread(&flags, 1, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
            return;
        }
        // Log the type in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Extension block %d", (int) flags);
            ppl_log(ec, msg);
        }
        // A graphic control extension may give a transparent color; others are skipped
        if ((int) flags == 249) {
            // Read the length of the extension
            if (fread(&flags, 1, 1, in) != 1) {
                ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
                return;
            }
            // It must be four bytes long
            if (flags != 4) {
                ppl_error(ec, ERR_FILE, -1, -1, "GIF image file has an unexpected extension length");
                return;
            }
            // Read its flags
            if (fread(&flags, 1, 1, in) != 1) {
                ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
                return;
            }
            // Read the transparent color index, skipping the delay time
            if (flags & 1) {
                image->trans = &trans;
                fseek(in, 2, SEEK_CUR);
                // Read the index
                if (fread(&flags, 1, 1, in) != 1) {
                    ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
                    return;
                }
                *image->trans = flags;
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Transparent colour index at %d", (int) flags);
                    ppl_log(ec, msg);
                }
            } else {
                fseek(in, 3, SEEK_CUR);
            }
            // Read the terminating zero
            if (fread(&flags, 1, 1, in) != 1) {
                ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
                return;
            }
            // Report a missing terminator
            if (flags) {
                ppl_error(ec, ERR_FILE, -1, -1, "GIF image file has unexpected extension data");
                return;
            }
        } else {
            // Skip each sub-block of the extension, until a zero-length one
            do {
                // Read the length of the sub-block
                if (!fread(&flags, 1, 1, in)) {
                    ppl_error(ec, ERR_FILE, -1, -1, "GIF image file ends prematurely");
                    return;
                }
                fseek(in, (long) flags, SEEK_CUR);
            } while (flags);
        }
        // Read the type of the next block
        if (fread(&flags, 1, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
            return;
        }
    }

    // An image descriptor should follow
    if ((flags == 0x2c) && DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Local descriptor");
        ppl_log(ec, msg);
    }

    // Read the offset of the image
    if (fread(buff, 4, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }
    lxoff = ((unsigned) buff[1] << 8) + buff[0];
    lyoff = ((unsigned) buff[3] << 8) + buff[2];

    // Log the offset in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Local offset %ldx%ld", lxoff, lyoff);
        ppl_log(ec, msg);
    }

    // Read the size of the image
    if (fread(buff, 4, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }
    lw = ((unsigned) buff[1] << 8) + buff[0];
    lh = ((unsigned) buff[3] << 8) + buff[2];

    // Log the size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Local size %ldx%ld", lw, lh);
        ppl_log(ec, msg);
    }

    // Read the flags describing the local color map
    if (fread(&flags, 1, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }

    // Read the local color map, which replaces the global one
    if (flags & 0x80) {
        // Log that there is a local color map in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Local colour map");
            ppl_log(ec, msg);
        }
        ncols = 2 << (flags & 0x7);
        // Log the number of colors in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Local number of colours=%d", ncols);
            ppl_log(ec, msg);
        }
        // Discard the global color map
        if (gcm) {
            free(image->palette);
            image->palette = NULL;
        }
        image->palette = ppl_memAlloc(ncols * 3);
        // Failure to allocate memory is reported as an error
        if (image->palette == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return;
        }
        // Read the colors
        if (fread(image->palette, ncols * 3, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
            return;
        }
        image->pal_len = ncols;
    }

    // Whether the rows are interlaced
    interlaced = flags & 0x40;

    // Log interlacing in debugging mode
    if (interlaced && DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Interlaced");
        ppl_log(ec, msg);
    }

    // The image must fill the logical screen
    if (lxoff || lyoff || (lw != width) || (lh != height)) {
        ppl_error(
                ec, ERR_FILE, -1, -1,
                "GIF image file cannot be processed because local and global sizes differ; file appears to be corrupt");
        return;
    }

    // Read the initial LZW code size; codes start one bit longer than this
    if (fread(&flags, 1, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        return;
    }
    lzwcs = flags + 1;
    // Log the code size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Initial code size=%ld", lzwcs);
        ppl_log(ec, msg);
    }

    // A buffer for the compressed data
    rawz = (unsigned char *) malloc((width * height * 3) / 2);
    // Failure to allocate memory is reported as an error
    if (rawz == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return;
    }

    // The compressed data come in sub-blocks of up to 255 bytes, each preceded by its length, and ending with an empty
    // one
    datalen = 0;
    // Read the length of the first sub-block
    if (fread(&len, 1, 1, in) != 1) {
        ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
        free(rawz);
        rawz = NULL;
        return;
    }
    // Read each sub-block
    while (len) {
        // Read the data
        if (fread(rawz + datalen, len, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
            free(rawz);
            rawz = NULL;
            return;
        }
        datalen += len;
        // Read the length of the next sub-block
        if (fread(&len, 1, 1, in) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This GIF image file appears to be corrupted");
            free(rawz);
            rawz = NULL;
            return;
        }
    }
    // Log the amount of data in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Total GIF data length=%ld", datalen);
        ppl_log(ec, msg);
    }

    // Allocate the decompressed image, one byte per pixel
    image->data = (unsigned char *) ppl_memAlloc(width * height);
    // Failure to allocate memory is reported as an error
    if (image->data == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        free(rawz);
        rawz = NULL;
        return;
    }

    // Decompress the data
    datalen = ppl_bmp_de_lzw(ec, rawz, image->data, width * height, lzwcs);
    free(rawz);
    rawz = NULL;

    // Decompression failed
    if (datalen == 0) {
        image->data = NULL;
        return;
    } // Subroutine failed
    // The decompressed data must fill the image
    if (datalen != width * height) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "Decoding error whilst processing GIF image file. Expecting %ld bytes of decoded data, but received %ld.",
                width * height, datalen);
        ppl_error(ec, ERR_FILE, -1, -1, msg);
        return;
    }

    // Record the size and type of the image
    image->data_len = width * height;
    image->height = height;
    image->width = width;
    image->type = BMP_COLOUR_PALETTE;
    image->colour = BMP_COLOUR_PALETTE;
    image->depth = 8;

    // Reorder the rows of interlaced images
    if (interlaced) {
        // Reorder the rows
        if (ppl_bmp_de_gifinterlace(ec, image)) {
            image->data = NULL;
            return;
        } // Subroutine failed
    }
    return;
}

//! ppl_bmp_de_gifinterlace - Reorder the rows of an interlaced GIF image into their natural order
//! \param [in] ec - The error-reporting context
//! \param [in] image - The image structure into which the image is read
//! \return Zero on success, or one if memory allocation failed

int ppl_bmp_de_gifinterlace(pplerr_context *ec, bitmap_data *image) {
    // Interlaced GIFs store every 8th row, then the rows halfway between them, then the quarter rows, then the odd rows
    int i, j;
    unsigned char *out, *in, *outp;

    // Allocate the reordered image
    out = (unsigned char *) ppl_memAlloc(image->height * image->width);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return 1;
    }

    // Copy the rows in the order they were stored
    in = image->data;

    // First pass is eights
    outp = out;
    // Rows 0, 8, 16, ...
    for (i = 0; i < image->height; i += 8) {
        for (j = 0; j < image->width; j++) *outp++ = *in++;
        outp += 7 * image->width;
    }

    // Next is fours
    outp = out + 4 * image->width;
    // Rows 4, 12, 20, ...
    for (i = 4; i < image->height; i += 8) {
        for (j = 0; j < image->width; j++) *outp++ = *in++;
        outp += 7 * image->width;
    }

    // Next is twos
    outp = out + 2 * image->width;
    // Rows 2, 6, 10, ...
    for (i = 2; i < image->height; i += 4) {
        for (j = 0; j < image->width; j++) *outp++ = *in++;
        outp += 3 * image->width;
    }

    // Next is ones
    outp = out + image->width;
    // Rows 1, 3, 5, ...
    for (i = 1; i < image->height; i += 2) {
        for (j = 0; j < image->width; j++) *outp++ = *in++;
        outp += image->width;
    }

    // Return the reordered image
    image->data = out;
    return 0;
}

#define MAXCS 12

//! ppl_bmp_de_lzw - Decompress the LZW-compressed pixel data of a GIF image
//! \param [in] ec - The error-reporting context
//! \param [in] buff - The compressed data
//! \param [out] out - Buffer into which to write the decompressed pixel data
//! \param [in] len - The size of the buffer <out>
//! \param [in] cs - The initial LZW code size, in bits
//! \return The number of bytes of decompressed data, or zero on failure

unsigned long ppl_bmp_de_lzw(pplerr_context *ec, unsigned char *buff, unsigned char *out, const unsigned long len,
                             int cs) {
    char msg[PPL_MSG_LENGTH];
    unsigned char store[256], *start, *end;
    unsigned int off, tpos, eoi, clr;
    struct str {
        unsigned char *s;
        unsigned len;
    } table[1 << MAXCS];
    int tmax = (1 << MAXCS) - 1, ccs, i, j;

    // The start and end of the output buffer
    start = out;
    end = out + len;

    // Codes may be no longer than MAXCS bits
    if (cs > MAXCS) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "Whilst decoding GIF image file, encountered de_lzw error: initial token size of %d too large", cs);
        ppl_error(ec, ERR_FILE, -1, -1, msg);
        return 0;
    }

    // Init table
    off = 0;
    ccs = cs;
    clr = (1 << (cs - 1));
    eoi = clr + 1;
    tpos = clr + 2;

    // The first entries of the string table are the single pixel values
    for (i = 0; i < clr; i++) {
        table[i].len = 1;
        table[i].s = store + i;
        store[i] = (unsigned char) i;
    }

    // The data must start with a clear code
    i = ppl_bmp_de_lzw_bits(ec, buff, 0, ccs);
    // Report data which do not start with a clear code
    if (i != clr) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "Whilst decoding GIF image file, encountered de_lzw error: ClearCode not first code, but instead got %x",
                i);
        ppl_error(ec, ERR_FILE, -1, -1, msg);
        return 0;
    }

    // Read each code; each new code adds the previous string plus one pixel to the table, which is filled in when the
    // next code is written
    while (1) {
        i = ppl_bmp_de_lzw_bits(ec, buff + (off >> 3), off & 7, ccs);
        off += ccs;

        // A clear code resets the table and the code size
        if (i == clr) {
            ccs = cs;
            tpos = clr + 2;
            continue;
        }

        // An end-of-information code ends the data
        if (i == eoi) return (out - start);

        // The code must be in the table
        if (i >= tpos) {
            ppl_snprintfTrunc(msg, sizeof(msg),
                              "Whilst decoding GIF image file, encountered de_lzw error: token erroneously large");
            ppl_error(ec, ERR_FILE, -1, -1, msg);
            return 0;
        }
        // The output must not overflow
        if (out > end) {
            ppl_snprintfTrunc(msg, sizeof(msg),
                              "Whilst decoding GIF image file, encountered de_lzw error: output buffer full");
            ppl_error(ec, ERR_FILE, -1, -1, msg);
            return 0;
        }

        // Add a new string to the table, which is the string for this code plus the first pixel of the next, which is
        // written immediately after it
        if (tpos <= tmax) {
            table[tpos].len = table[i].len + 1;
            table[tpos].s = out;
            tpos++;
        }

        // Write the string for this code
        for (j = 0; (j < table[i].len) && (out <= end); j++) *(out++) = *(table[i].s + j);

        // Lengthen the codes when the table outgrows them
        if ((tpos == (1 << ccs) + 1) && (ccs < MAXCS)) ccs++;
    }
    // Should never get here
    ppl_snprintfTrunc(msg, sizeof(msg), "Whilst decoding GIF image file, encountered unidentified de_lzw error");
    ppl_error(ec, ERR_FILE, -1, -1, msg);
    return 0;
}

//! ppl_bmp_de_lzw_bits - Extract an LZW code from a stream of packed bits
//! \param [in] ec - The error-reporting context
//! \param [in] c - The packed bit stream
//! \param [in] st - The position of the first bit of the code within the stream
//! \param [in] len - The length of the code, in bits
//! \return The code

unsigned int ppl_bmp_de_lzw_bits(const pplerr_context *ec, unsigned char *c, const int st, const int len) {
    unsigned tmp, mask;

    // Read three bytes, which contain the code, and extract it
    tmp = ((unsigned) (*(c + 2)) << 16) + ((unsigned) (*(c + 1)) << 8) + *(c);
    mask = ((1 << len) - 1) << st;
    tmp &= mask;
    return (tmp >> st);
}
