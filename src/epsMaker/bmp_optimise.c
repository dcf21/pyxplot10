// bmp_optimise.c
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

// Reduces the size of bitmap images before they are written into postscript by eps_image.c. RGB images with no more
// than 256 colors are converted to paletted images; unused palette entries are dropped; paletted images whose colors
// are all grey are converted to greyscale; and images with few colors are packed with 1, 2 or 4 bits per pixel. Edited
// from code kindly contributed to Pyxplot by Michael Rutter.

#define _PPL_BMP_OPTIMISE_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "coreUtils/memAlloc.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/errorReport.h"

#include "epsMaker/bmp_optimise.h"
#include "epsMaker/bmp_image.h"

//! ppl_bmp_colour_count - Count the number of distinct colors in an RGB image, and if there are few enough, convert it
//! to a paletted image to reduce its size
//! \param [in] ec - The error-reporting context
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_colour_count(pplerr_context *ec, bitmap_data *image) {
    // The colors found so far, as 24-bit numbers; one more than a palette can hold
    char msg[PPL_MSG_LENGTH];
    unsigned long size, i;
    int ncols, j, colour;
    int palette[257];
    unsigned char *p;

    // Count the distinct colors, stopping once there are too many for a palette
    size = (long) image->height * (long) image->width;
    ncols = 0;
    p = image->data;

    // Count the number of colours in the image
    for (i = 0; i < size; i++) {
        colour = (((int) *p) << 16) + (((int) *(p + 1)) << 8) + *(p + 2);
        p += 3;
        for (j = 0; j < ncols; j++)
            if (colour == palette[j]) break;
        // A new color
        if (j == ncols) {
            palette[ncols++] = colour;
            if (ncols == 257) break;
        }
    }

    // Too many colors for a palette
    if (ncols > 256) {
        if (DEBUG) ppl_log(ec, "Image contains more than 256 colours");
        return;
    }

    // Log the number of colors in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Image contains only %d colours: reducing to paletted image", ncols);
        ppl_log(ec, msg);
    }

    // Reduce to paletted image if possible
    image->palette = (unsigned char *) ppl_memAlloc(3 * ncols);
    // Failure to allocate memory is reported as an error
    if (image->palette == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return;
    }

    // Copy colours we found into palette
    for (i = 0; i < ncols; i++) {
        image->palette[3 * i] = (palette[i] & 0xff0000) >> 16;
        image->palette[3 * i + 1] = (palette[i] & 0xff00) >> 8;
        image->palette[3 * i + 2] = (palette[i] & 0xff);
    }

    // Replace RGB data with paletted colours
    p = image->data;
    // Replace each pixel's three bytes with one byte giving its palette entry, in place
    for (i = 0; i < size; i++) {
        colour = (((int) p[3 * i]) << 16) + (((int) p[3 * i + 1]) << 8) + p[3 * i + 2];
        for (j = 0; j < ncols; j++)
            if (colour == palette[j]) break;
        p[i] = j;
    }

    // Replace transparent colour with paletted colour
    if (image->trans != NULL) {
        p = image->trans;
        colour = (((int) p[0]) << 16) + (((int) p[1]) << 8) + p[2];
        for (j = 0; j < ncols; j++)
            if (colour == palette[j]) break;
        if (j == ncols) image->trans = NULL; // Transparent colour not present in image
        else p[0] = j;
    }

    // Set up image headers to show that it is now a paletted image
    image->data_len = size;
    image->depth = 8;
    image->colour = BMP_COLOUR_PALETTE;
    image->type = BMP_COLOUR_PALETTE;
    image->pal_len = ncols;

    // Done
    return;
}

//! ppl_bmp_palette_check - Shorten the palette of an 8-bit paletted image if its highest entries are not used, and
//! drop the transparent color if it is one of those entries
//! \param [in] ec - The error-reporting context
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_palette_check(pplerr_context *ec, bitmap_data *image) {
    char msg[PPL_MSG_LENGTH];
    int i;
    unsigned long size;
    unsigned char ncols;

    // Only 8-bit paletted images are checked
    if (image->type != BMP_COLOUR_PALETTE) return;
    if (image->depth != 8) return;

    // Find highest used palette entry
    ncols = 0;
    size = image->data_len;
    for (i = 0; i < size; i++) ncols = (ncols > image->data[i] ? ncols : image->data[i]);

    // Shorten the palette, if its last entries are unused
    if (ncols < (image->pal_len - 1)) {
        // Log the new palette length in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Palette length reduced from %d to %d", image->pal_len, 1 + ncols);
            ppl_log(ec, msg);
        }
        image->pal_len = 1 + ncols;
        if ((image->trans != NULL) && (image->trans[0] > ncols))
            image->trans = NULL; // Transparent colour is not used in image
    }
    return;
}

//! ppl_bmp_grey_check - Check whether all of the colors in an 8-bit paletted image are shades of grey, and if so
//! convert it to a greyscale image, with fewer bits per pixel if the greys are evenly spaced
//! \param [in] ec - The error-reporting context
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_grey_check(pplerr_context *ec, bitmap_data *image) {
    // Variables describing the greyscale image
    char msg[PPL_MSG_LENGTH];
    int i, grey, depth = 8, magic, test;
    unsigned int ncols;
    unsigned long size;

    // Only 8-bit paletted images are checked
    if (image->type != BMP_COLOUR_PALETTE) return;
    if (image->depth != 8) return;

    // The number of palette entries and pixels
    ncols = image->pal_len;
    size = image->data_len;

    // See if palette is greyscale. If not, we cannot reduce it to greyscale.
    grey = 1;
    for (i = 0; i < image->pal_len; i++)
        if ((image->palette[3 * i] != image->palette[3 * i + 1]) ||
            (image->palette[3 * i] != image->palette[3 * i + 2])) {
            grey = 0;
            break;
        }
    if (!grey) return;

    // Log that the image is greyscale in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Image is greyscale");
        ppl_log(ec, msg);
    }

    if (ncols <= 16) // Investigate whether we can reduce colour depth below 8
    {
        if (ncols == 2) // We have only two colours
        {
            magic = 255;
            depth = 1;
        } else if (ncols <= 4) // We have only four colours
        {
            magic = 255 / 3;
            depth = 2;
        } else // We have sixteen colours
        {
            magic = 255 / 15;
            depth = 4;
        }
        test = 1; // Test whether the colours we have are all evenly spaced between 0 and 255
        // Check that every grey is a whole number of steps
        for (i = 0; i < ncols; i++) {
            // This grey is not a whole number of steps
            if ((image->palette[3 * i] % magic) != 0) {
                test = 0;
                break;
            }
        }
        if (test) // If so, we can reduce colour depth
        {
            for (i = 0; i < ncols; i++) image->palette[3 * i] /= magic;
            // Log the depth in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Greyscale depth is %d bit", depth);
                ppl_log(ec, msg);
            }
        } else { // We have a <=4 bit image, but cannot use a 4-bit greyscale representation. Give up and use palette!
            // Log why the image cannot be reduced in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg), "ncols=%d, but not %d bit greyscale\n", ncols, depth);
                ppl_log(ec, msg);
            }
            return;
        }
    }

    // See if palette is already ordered
    for (i = 0; i < image->pal_len; i++)
        // Check whether each palette entry is already its own grey level
        if (image->palette[3 * i] != i) {
            grey = 0;
            break;
        }

    if (!grey) // Need to order things
    {
        if (DEBUG) ppl_log(ec, "Greyscale palette being reordered");
        for (i = 0; i < size; i++) image->data[i] = image->palette[3 * image->data[i]];
        if (image->trans != NULL)
            image->trans[0] = image->palette[3 * image->trans[0]]; // Reorder transparent colour as well
    }

    // Now we can ditch palette etc.
    image->palette = NULL;
    // Pack greyscale images of fewer than 8 bits per pixel
    if (depth <= 4) {
        image->pal_len = 1 << depth; // Fudge settings so that ppl_bmp_compact is happy
        image->type = BMP_COLOUR_PALETTE;
        ppl_bmp_compact(ec, image);
    }
    image->type = BMP_COLOUR_BMP;
    image->colour = BMP_COLOUR_GREY;
    image->pal_len = 0;
    return;
}

//! ppl_bmp_compact - Reduce the bit depth of a paletted or greyscale image to the minimum needed to represent its
//! colors, packing several pixels into each byte
//! \param [in] ec - The error-reporting context
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_compact(pplerr_context *ec, bitmap_data *image) {
    // Variables for packing the pixels
    int ncols, i, j, height, width;
    unsigned char *p, *p2, ctmp;

    // Only 8-bit paletted images with no more than 16 colors can be packed
    if (image->type != BMP_COLOUR_PALETTE) return;
    if (image->depth != 8) return;
    if (image->pal_len > 16) return;

    // The number of colors determines the number of bits per pixel
    ncols = image->pal_len;
    height = image->height;
    width = image->width;

    // Pack two-color images with 1 bit per pixel, 2-color to 4-color ones with 2 bits, and up to 16 colors with 4 bits;
    // each row starts on a new byte
    if (ncols == 2) {
        if (DEBUG) ppl_log(ec, "Compacting to 1 bit per pixel");
        image->depth = 1;
        p = p2 = image->data;
        // Pack each row, eight pixels per byte
        for (i = 0; i < height; i++) {
            for (j = 0; j < width - 7; j += 8, p2 += 8)
                *p++ = (*p2 << 7) + (*(p2 + 1) << 6) + (*(p2 + 2) << 5) + (*(p2 + 3) << 4) + (*(p2 + 4) << 3) +
                        (*(p2 + 5) << 2) + (*(p2 + 6) << 1) + *(p2 + 7);
            // Pack the pixels left at the end of the row into a final byte
            if (j != width) {
                *p = 0;
                for (; j < width; j++) *p = (*p << 1) + *p2++;
                *p = *p << (8 - (width & 7));
                ctmp = *(p - 1) & ((1 << (8 - (width & 7))) - 1); // this might help the rle
                *p |= ctmp; // ditto
                p++;
            }
        }
        image->data_len = ((long) height) * ((width + 7) >> 3);
    } else if (ncols <= 4) {
        if (DEBUG) ppl_log(ec, "Compacting to 2 bits per pixel");
        image->depth = 2;
        p = p2 = image->data;
        // Pack each row, four pixels per byte
        for (i = 0; i < height; i++) {
            for (j = 0; j < width - 3; j += 4, p2 += 4)
                *p++ = (*p2 << 6) + (*(p2 + 1) << 4) + (*(p2 + 2) << 2) + *(p2 + 3);
            if (j != width) {
                *p = 0;
                for (; j < width; j++) *p = (*p << 2) + *p2++;
                *p = *p << (2 * (4 - (width & 3)));
                ctmp = *(p - 1) & ((1 << (2 * (4 - (width & 3)))) - 1); // this might help the rle
                *p |= ctmp; // ditto
                p++;
            }
        }
        image->data_len = ((long) height) * ((width + 3) >> 2);
    } else if (ncols <= 16) {
        if (DEBUG) ppl_log(ec, "Compacting to 4 bits per pixel");
        image->depth = 4;
        p = p2 = image->data;
        // Pack each row, two pixels per byte
        for (i = 0; i < height; i++) {
            for (j = 0; j < (width - 1); j += 2, p2 += 2) *(p++) = (*p2 << 4) + *(p2 + 1);
            // An odd pixel at the end of the row has a byte of its own
            if (j == (width - 1)) {
                *p = (*(p2++)) << 4;
                *p |= *(p - 1) & 0xf; // to help rle
                p++;
            }
        }
        image->data_len = ((long) height) * ((width + 1) >> 1);
    }

    // Done
    return;
}
