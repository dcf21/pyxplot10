// bmp_jpegread.c
//
// The code in this file is part of Pyxplot
// <http://www.pyxplot.org.uk>
//
// Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
//               2008-2013 Ross Church
//
//               2009-2012 Michael Rutter
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

// Reads JPEG image files for the image command (eps_image.c) without decompressing them: the compressed (DCT-encoded)
// data are written straight into the postscript, which the postscript interpreter decodes. Only the header is parsed,
// to find the image's size and number of color components, and the segments needed for decoding are kept while others,
// such as thumbnails and comments, are discarded. Progressive JPEGs are not supported. Edited from code kindly
// contributed to Pyxplot by Michael Rutter.

#define _PPL_BMP_JPEGREAD_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <gsl/gsl_const_mksa.h>

#include "coreUtils/memAlloc.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/errorReport.h"

#include "epsMaker/bmp_image.h"
#include "epsMaker/bmp_jpegread.h"

#define HEADLEN 8 * 1024

//! ppl_bmp_jpegread - Read a JPEG image file. The compressed data is kept as it is, to be embedded directly in
//! postscript with a DCT filter; only the header is decoded, to determine the image's dimensions and color space.
//! \param [in] ec - The error-reporting context
//! \param [in] jpeg - The file handle of the JPEG image
//! \param [in] image - The image structure into which the image is read

void ppl_bmp_jpegread(pplerr_context *ec, FILE *jpeg, bitmap_data *image) {
    // Variables describing the image
    char msg[PPL_MSG_LENGTH];
    int comps = 0, i, j;

    // Buffers for the image data, and for the header segments which are kept
    unsigned int width = 0, height = 0, save, sos;
    unsigned int len, chunk = 64 * 1024;
    unsigned char *buff, *header, *headp, *headendp, type = 0, comp = 0, *p;

    // Allocate the buffers
    buff = (unsigned char *) ppl_memAlloc(chunk);
    header = (unsigned char *) malloc(HEADLEN);
    headp = header;
    headendp = header + HEADLEN - 8;

    // Log progress in debugging mode
    if (DEBUG) ppl_log(ec, "Beginning to decode JPEG image file");

    // Failure to allocate memory is reported as an error
    if ((buff == NULL) || (header == NULL)) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        free(header);
        header = NULL;
        return;
    }

    *(headp++) = 0xff;
    *(headp++) = 0xd8;

    // Read each segment of the header, up to the start of the image data (marker 0xda)
    do {
        // Read the segment's type and length
        if (fread(buff, 3, 1, jpeg) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This JPEG image file appears to be corrupted (A)");
            free(header);
            header = NULL;
            return;
        }
        type = buff[0];
        len = buff[1] * 256 + buff[2] - 2;
        save = 0;
        // Read the segment
        if (fread(buff, len, 1, jpeg) != 1) {
            ppl_error(ec, ERR_FILE, -1, -1, "This JPEG image file appears to be corrupted (B)");
            free(header);
            header = NULL;
            return;
        }
        // Log each segment in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Entry type %x length 0x%x", (int) type, len + 2);
            ppl_log(ec, msg);
        }
        // A JFIF segment gives the image's resolution
        if ((type == 0xe0) && !strcmp((char *) buff, "JFIF")) {
            // The resolution is in dots per inch or per centimetre
            if (((buff[7] == 1) || (buff[7] == 2))) {
                image->XDPI = buff[8] * 256 + buff[9];
                image->YDPI = buff[10] * 256 + buff[11];
                if (buff[7] == 2) /* units were pixels per cm */
                {
                    image->XDPI *= 25.4;
                    image->YDPI *= 25.4;
                }
            }
            // Log the JFIF information in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg), "JPEG version %d.%02d", (int) buff[5], (int) buff[6]);
                ppl_log(ec, msg);
                ppl_snprintfTrunc(msg, sizeof(msg), "JFIF thumbnail size %dx%d", (int) buff[12], (int) buff[13]);
                ppl_log(ec, msg);
                // Log the resolution, if known
                if (image->XDPI) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "DPI: %.1fx%.1f", image->XDPI, image->YDPI);
                    ppl_log(ec, msg);
                }
                ppl_snprintfTrunc(msg, sizeof(msg), "JFIF APP0 entry length 0x%x", len + 2);
                ppl_log(ec, msg);
            }
        }

        // Log Exif segments in debugging mode
        if (DEBUG && ((type == 0xe1) && (!strcmp((char *) buff, "Exif")))) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Exif JPEG file");
            ppl_log(ec, msg);
            ppl_snprintfTrunc(msg, sizeof(msg), "Exif APP1 entry length 0x%x", len + 2);
            ppl_log(ec, msg);
        }

        // A start-of-frame segment gives the image's size, number of components and encoding
        if (((type & 0xf0) == 0xc0) && (type != 0xc4) && (type != 0xcc)) {
            // There may only be one image
            if (comp) {
                ppl_error(ec, ERR_FILE, -1, -1, "Cannot decode JPEG image file: it contains multiple images?");
                free(header);
                header = NULL;
                return;
            }
            comp = type;
            height = 256 * buff[1] + buff[2];
            width = 256 * buff[3] + buff[4];
            comps = buff[5];
            save = 1;
        }

        // Log comments in debugging mode
        if (DEBUG && (type == 0xfe)) {
            buff[len] = 0;
            ppl_snprintfTrunc(msg, sizeof(msg), "JPEG Comment: %s", buff);
            ppl_log(ec, msg);
        }
        // Log APP0 markers in debugging mode
        if (DEBUG && (type == 0xe0)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "APP0 Marker: %s", buff);
            ppl_log(ec, msg);
        }
        // Log restart markers in debugging mode
        if (DEBUG && (type == 0xdd)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Restart markers present");
            ppl_log(ec, msg);
        }

        // Keep the quantisation and Huffman tables, the start of scan and the restart interval
        if ((type == 0xdb) || (type == 0xc4) || (type == 0xda) || (type == 0xdd)) save = 1;
        // Copy the segment into the header, or discard it
        if (save) {
            *(headp++) = 0xff;
            *(headp++) = type;
            *(headp++) = (len + 2) >> 8;
            *(headp++) = (len + 2) & 0xff;
            if (headp + len > headendp) {
                ppl_error(ec, ERR_FILE, -1, -1, "JPEG header storage exhausted");
                free(header);
                header = NULL;
                return;
            }
            for (i = 0; i < len; i++) *(headp++) = buff[i];
        } else if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Discarding section of JPEG image file.");
            ppl_log(ec, msg);
        }
    } while ((type != 0xda) && fread(buff, 1, 1, jpeg) && (buff[0] == 0xff));

    // Log the image size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Image size %dx%d with %d components", width, height, comps);
        ppl_log(ec, msg);
    }

    // Only baseline and extended sequential encodings are supported
    switch (comp) {
        case 0xc0:
            if (DEBUG) ppl_log(ec, "Encoding: baseline JPEG");
            break;
        case 0xc1:
            if (DEBUG) ppl_log(ec, "Encoding: extended sequential, Huffman JPEG");
            break;
        case 0xc2:
            ppl_error(
                    ec, ERR_FILE, -1, -1,
                    "JPEG image detected to have progressive encoding, which Pyxplot does not support. Please convert to baseline JPEG and try again.");
            free(header);
            header = NULL;
            return;
        default:
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "JPEG image detected to have unsupported compression type SOF%d. Please convert to baseline JPEG and try again.",
                    ((int) comp) & 0xf);
            ppl_error(ec, ERR_FILE, -1, -1, msg);
            free(header);
            header = NULL;
            return;
    }

    // Only greyscale and RGB images are supported
    if ((comps != 3) && (comps != 1)) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "JPEG image contains %d colour components; Pyxplot only supports JPEG images with one (greyscale) or three (RGB) components",
                comps);
        ppl_error(ec, ERR_FILE, -1, -1, msg);
        free(header);
        header = NULL;
        return;
    }

    // Three components are RGB; one is greyscale
    if (comps == 3) {
        image->colour = BMP_COLOUR_RGB;
        image->depth = 24;
    } else {
        image->colour = BMP_COLOUR_GREY;
        image->depth = 8;
    }

    // Record the image's size, and that its data are DCT-encoded
    image->width = width;
    image->height = height;
    image->TargetCompression = BMP_ENCODING_DCT;

    // Now read JPEG image data. Unfortunately, we don't know how much we'll get

    // Log the size of the header in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "%d bytes of header read", (int) (headp - header));
        ppl_log(ec, msg);
    }

    // Put the header at the start of the data buffer, followed by the rest of the file
    for (i = 0; header + i < headp; i++) buff[i] = header[i];
    len = chunk - (headp - header);
    free(header);
    header = NULL;
    j = fread(buff + i, 1, len, jpeg);

    // If the file fitted in the buffer, we are done; otherwise keep doubling the buffer until it does
    if (j < len) {
        image->data_len = i + j;
    } else {
        i = j;
        // Double the size of the buffer, and read more
        while (i == len) {
            chunk *= 2;
            p = (unsigned char *) ppl_memAlloc(
                    chunk); // We didn't malloc enough memory, and ppl_memAlloc can't realloc, so just have to malloc a new chunk :(
            // Failure to allocate memory is reported as an error
            if (p == NULL) {
                ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
                return;
            }
            memcpy(p, buff, chunk / 2);
            buff = p;
            len = chunk / 2;
            i = fread(buff + len, 1, len, jpeg);
        }
        image->data_len = len + i;
    }

    // Check what we have read for integrity and trailing rubbish. Assume that
    // the first two bytes are 0xffd8. Then we need not deal with awkward markers
    // which are not followed by lengths...

    // Check the structure of the data, skipping the start-of-image marker
    i = 2;
    sos = 0;

    // Step through each segment
    while (i < image->data_len) {
        // Each segment must start with a marker
        if (buff[i] != 0xff) {
            // Log the offset in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Offset %d", i);
                ppl_log(ec, msg);
            }
            ppl_error(ec, ERR_FILE, -1, -1, "Unexpected data in JPEG - no marker where expected");
            return;
        }
        i++;
        // Log each segment in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Check: entry type %x ...", buff[i]);
            ppl_log(ec, msg);
        }
        if (buff[i] == 0xd9) break; // End of image
        if (buff[i] == 0xda) sos = 1;
        // A second start-of-image marker means the file is corrupt
        if (buff[i] == 0xd8) {
            ppl_error(ec, ERR_FILE, -1, -1, "Unexpected extra SOI in JPEG data.");
            return;
        }
        i++;
        len = buff[i] * 256 + buff[i + 1] - 2;
        i += 2;
        // Log the segment's length in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "... length 0x%x", len + 2);
            ppl_log(ec, msg);
        }
        i += len;
        if (sos) // We now have an entropy-encoded section of unknown length
        {
            j = i;
            // Entropy-encoded sections stop at a marker (0xff), save that they
            // don't for 0xff00, and it is easiest to consider the restart markers
            // 0xffd0 to 0xffd7 to be part of the entropy-encoded section
            while (!((i >= image->data_len) ||
                     ((buff[i] == 0xff) && ((buff[i + 1] != 0) && ((buff[i + 1] & 0xf8) != 0xd0)))))
                i++;
            // Log the length of the entropy-encoded data in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Check: entropy encoded section %d bytes", i - j);
                ppl_log(ec, msg);
            }
            sos = 0;
        }
    }

    // The data must end with an end-of-image marker
    if (buff[i] != 0xd9) {
        ppl_error(ec, ERR_FILE, -1, -1, "Error -- JPEG EOI not found, file corrupted/truncated?");
        return;
    }

    // Log any trailing data, which is discarded
    if (DEBUG && (i + 1 < image->data_len)) {
        ppl_snprintfTrunc(msg, sizeof(msg), "%lu bytes of trailing garbage removed from JPEG", image->data_len - i - 1);
        ppl_log(ec, msg);
    }

    // Return the data, up to and including the end-of-image marker
    image->data = buff;
    image->data_len = i + 1;
    return;
}
