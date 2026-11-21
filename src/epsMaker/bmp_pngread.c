// bmp_pngread.c
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

// Reads PNG image files for the image command (eps_image.c) using libpng, decoding them into the bitmap_data structure
// used by Pyxplot's image code. 16-bit channels are reduced to 8 bits, alpha channels are blended onto the image's
// background color (or white), and a single fully transparent palette entry or color is kept as the image's transparent
// color. Edited from code kindly contributed to Pyxplot by Michael Rutter.

#define _PPL_BMP_PNGREAD_C 1

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <png.h>

#include "coreUtils/memAlloc.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/errorReport.h"

#include "epsMaker/bmp_image.h"
#include "epsMaker/bmp_pngread.h"

//! ppl_bmp_pngread - Read a PNG image file using libpng. On failure, an error is reported and <image->data> is set to
//! NULL.
//! \param [in] ec - The error-reporting context
//! \param [in] in - The file handle of the PNG image
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_pngread(pplerr_context *ec, FILE *in, bitmap_data *image) {
    // Variables describing the image
    char msg[PPL_MSG_LENGTH];
    int ncols, ntrans, i;
    static unsigned char index[3];
    png_bytep trans_colours;
    png_color_16p trans_val;
    png_color_16p backgndp;
    png_color_16 background;
    png_colorp palette;

    // Log progress in debugging mode
    if (DEBUG) ppl_log(ec, "Beginning to decode PNG image file");

    // Initialise libpng data structures
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    // Failure to allocate memory is reported as an error
    if (png_ptr == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return;
    }

    // Create libpng's description of the image
    png_infop info_ptr = png_create_info_struct(png_ptr);
    // Failure to allocate memory is reported as an error
    if (info_ptr == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return;
    }

    // libpng reports errors by jumping back to here
    if (setjmp(png_jmpbuf(png_ptr))) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Unexpected error in libpng while trying to decode PNG image file");
        return;
    }

    // Read from the file, whose first three bytes have already been read to identify it
    png_init_io(png_ptr, in);
    png_set_sig_bytes(png_ptr, 3); // Three bytes consumed before this routine was called

    // Let libpng read header, and deal with outcome
    png_read_info(png_ptr, info_ptr);

    // Read the size and type of the image
    unsigned width = png_get_image_width(png_ptr, info_ptr);
    unsigned height = png_get_image_height(png_ptr, info_ptr);
    int depth = png_get_bit_depth(png_ptr, info_ptr);
    const int png_colour_type = png_get_color_type(png_ptr, info_ptr);

    // Log the size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Size %dx%d", width, height);
        ppl_log(ec, msg);
    }
    // Log the depth in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Depth %d", depth);
        ppl_log(ec, msg);
    }

    // Reduce 16-bit channels to 8 bits
    if (depth == 16) {
        if (DEBUG) ppl_log(ec, "Reducing 16 bit per components to 8 bits");
        png_set_strip_16(png_ptr);
        depth = 8;
    }

    // Blend an alpha channel onto the image's background color, or onto white
    if (png_colour_type & PNG_COLOR_MASK_ALPHA) {
        if (DEBUG) ppl_log(ec, "PNG uses transparency");
        // Use the background color defined in the file, if there is one
        if (png_get_bKGD(png_ptr, info_ptr, &backgndp)) {
            if (DEBUG) ppl_log(ec, "PNG defines a background colour");
            png_set_background(png_ptr, backgndp, PNG_BACKGROUND_GAMMA_FILE, 1, 1.0);
        } else {
            if (DEBUG) ppl_log(ec, "PNG does not define a background colour");
            background.red = background.green = background.blue = background.gray =
                    0xff; // Define background colour to be white
            png_set_background(png_ptr, &background, PNG_BACKGROUND_GAMMA_FILE, 0, 1.0);
        }
    }

    // Greyscale or RGB images
    if ((png_colour_type == PNG_COLOR_TYPE_GRAY) || (png_colour_type == PNG_COLOR_TYPE_GRAY_ALPHA))
        image->colour = BMP_COLOUR_GREY;
    if ((png_colour_type == PNG_COLOR_TYPE_RGB) || (png_colour_type == PNG_COLOR_TYPE_RGB_ALPHA))
        image->colour = BMP_COLOUR_RGB;

    // Paletted images: copy the palette
    if (png_colour_type == PNG_COLOR_TYPE_PALETTE) {
        image->type = BMP_COLOUR_PALETTE;
        image->colour = BMP_COLOUR_PALETTE;
        i = png_get_PLTE(png_ptr, info_ptr, &palette, &ncols);
        // Report a paletted image with no palette
        if (i == 0) {
            ppl_error(ec, ERR_FILE, -1, -1, "PNG image file claims to be paletted, but no palette was found");
            return;
        }
        image->pal_len = ncols;
        image->palette = (unsigned char *) ppl_memAlloc(ncols * 3);
        // Failure to allocate memory is reported as an error
        if (image->palette == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return;
        }
        // Copy each color of the palette
        for (i = 0; i < ncols; i++) {
            *(image->palette + 3 * i) = palette[i].red;
            *(image->palette + 3 * i + 1) = palette[i].green;
            *(image->palette + 3 * i + 2) = palette[i].blue;
        }
        // Log the size of the palette in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "PNG image file contains a palette of %d colours", ncols);
            ppl_log(ec, msg);
        }
    } else {
        image->type = BMP_COLOUR_BMP;
    }

    // Update png info to reflect any requested conversions (e.g. 16 bit to 8 bit or Alpha to non-alpha
    png_read_update_info(png_ptr, info_ptr);

    // Now rowbytes will reflect what we will get, not what we had originally
    unsigned row_bytes = png_get_rowbytes(png_ptr, info_ptr);

    // Allocate block of memory for uncompressed image
    image->data = (unsigned char *) ppl_memAlloc(row_bytes * height);
    // Failure to allocate memory is reported as an error
    if (image->data == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return;
    }
    // libpng does not write the unused bits at the end of each row of images with less than eight bits per pixel, so
    // zero them, to make the compressed output reproducible
    memset(image->data, 0, row_bytes * height);

    // libpng requires a separate pointer to each row of image
    png_bytep *row_ptrs = (png_bytep *) ppl_memAlloc(height * sizeof(png_bytep));
    // Failure to allocate memory is reported as an error
    if (row_ptrs == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        image->data = NULL;
        return;
    }

    // Point to each row of the image
    for (i = 0; i < height; i++) row_ptrs[i] = image->data + row_bytes * i;

    // Get uncompressed image
    png_read_image(png_ptr, row_ptrs);

    // Free everything we don't need
    png_read_end(png_ptr, NULL);

    // Deal with transparency (we can only support images with single transparent palette entries)
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        if (DEBUG) ppl_log(ec, "PNG has transparency");
        png_get_tRNS(png_ptr, info_ptr, &trans_colours, &ntrans, &trans_val);
        // Log the number of transparent entries in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "PNG has %d transparent entries in palette", ntrans);
            ppl_log(ec, msg);
        }
        // In a paletted image, count the fully transparent entries; partially transparent ones count as many
        if (png_colour_type == PNG_COLOR_TYPE_PALETTE) {
            // We can cope with just one, fully transparent, entry in palette
            int j = 0;
            for (i = 0; i < ntrans; i++)
                if (trans_colours[i] == 0) j++;
                else if (trans_colours[i] != 255) j += 10;
            // Warn about transparency which cannot be represented, or note the transparent entry
            if (j != 1) {
                ppl_warning(
                        ec, ERR_FILE,
                        "PNG has transparency, but not in the form of a single fully colour in its palette. Such transparency is not supported by Pyxplot.");
            } else {
                for (i = 0; (i < ntrans) && (trans_colours[i] == 255); i++);
                image->trans = index;
                *image->trans = i;
            }
        } else {
            image->trans = index;
            *image->trans = trans_val->gray;
            if (image->colour == BMP_COLOUR_RGB) {
                image->trans[0] = trans_val->red;
                image->trans[1] = trans_val->green;
                image->trans[2] = trans_val->blue;
            }
        }
    }
    png_destroy_read_struct(&png_ptr, &info_ptr, (png_infopp) NULL);

    // Put all necessary information into the output data structure
    image->data_len = ((long) row_bytes) * height;
    image->height = height;
    image->width = width;
    image->depth = depth;
    if (image->colour == BMP_COLOUR_RGB) image->depth *= 3;
    return;
}
