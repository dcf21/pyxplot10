// pyxplot_fitshelper.c
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

// The pyxplot_fitshelper program, which is built and installed alongside Pyxplot. Pyxplot uses it as an input filter to
// read FITS files: it writes out each extension's headers, images and tables as plain text, which Pyxplot then reads
// like any other data file. If cfitsio was not available when Pyxplot was built, it is a dummy which reports that FITS
// files are not supported.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef HAVE_FITSIO
#include <fitsio.h>
#endif

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

// Required by the shared string-handling code; this program never sets it
int cancellationFlag = 0;

//! main - Entry point for pyxplot_fitshelper, an input filter which converts FITS data files into plain text which
//! Pyxplot can read
//! \param [in] argc - The number of command-line arguments
//! \param [in] argv - The command-line arguments: the filename of the FITS file to convert
//! \return The exit status of the process

int main(const int argc, char **argv) {
    // Buffers for the help and version text, and a warning shown if FITS support is missing
    char help_string[LSTR_LENGTH], version_string[FNAME_LENGTH], version_string_underline[FNAME_LENGTH],
            warntext[FNAME_LENGTH];
    const char *filename = NULL;
    int HaveFilename = 0;
#ifdef HAVE_FITSIO
    fitsfile *fptr = NULL;
    int status = 0, hdupos, hdutype, nkeys, bitpix, naxis, ncols, anynull = 0, dispwidth[16384];
    long naxes[256], nrows;
    char card[FLEN_CARD], keyword[FLEN_KEYWORD], colname[FLEN_VALUE], nullstr[] = "nan", tabval[16384] = "\0", *valptr;
#endif

#ifndef HAVE_FITSIO
    strcpy(warntext, "*** WARNING ***\nAs the cfitsio library was not installed on this computer when Pyxplot was\n\
installed, Pyxplot has been compiled with no support for fits files. This\n\
application is a dummy filter which does nothing.\n\n");
#else
    strcpy(warntext, "");
#endif

    // Make help and version strings
    ppl_snprintfTrunc(version_string, sizeof(version_string), "\nPyxplot Fits Helper %s\n\n", VERSION);

    // The text shown by the --help switch
    ppl_snprintfTrunc(help_string, sizeof(help_string), "\nPyxplot Fits Helper %s\n\
%s\n\
\n\
%sUsage: pyxplot_fitshelper <filename>\n\
  -h, --help:       Display this help.\n\
  -v, --version:    Display version number.\n\
\n\
This application is used as in input filter to convert tables of data in FITS\n\
format into ASCII for reading into Pyxplot. It is the filter which Pyxplot uses\n\
when asked to plot a .fits file, but is also generally useful whenever it is\n\
desired to convert data from FITS format into plain text.\n\
\n\
A brief introduction to Pyxplot can be obtained by typing 'man pyxplot'; the\n\
full Users' Guide can be found in the file:\n\
%s%spyxplot.pdf\n\
\n\
For the latest information on Pyxplot development, see the project website:\n\
<http://www.pyxplot.org.uk>\n\n",
                      VERSION, ppl_strUnderline(version_string, version_string_underline), warntext, DOCDIR, PATHLINK);

    // Scan command-line options for any switches
    HaveFilename = 0;
    // Process each command-line argument in turn
    for (int i = 1; i < argc; i++) {
        if (strlen(argv[i]) == 0) continue;
        // Arguments which are not switches are the filename to convert
        if (argv[i][0] != '-') {
            HaveFilename++;
            filename = argv[i];
            continue;
        }
        // Switches: version and help text are printed, and anything else is an error
        if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "-version") == 0) || (strcmp(argv[i], "--version") == 0)) {
            printf("%s", version_string);
            return 0;
        } else if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "-help") == 0) ||
                   (strcmp(argv[i], "--help") == 0)) {
            printf("%s", help_string);
            return 0;
        } else {
            fprintf(stderr,
                    "\nReceived switch '%s' which was not recognised.\nType 'pyxplot_fitshelper -help' for a list of available command-line options.\n\n",
                    argv[i]);
            return 1;
        }
    }

    // Check that we have been provided with exactly one filename on the command line
    if (HaveFilename < 1) {
        fprintf(stderr,
                "\npyxplot_fitshelper should be provided with a filename on the command line to\nact upon. Type 'pyxplot_fitshelper -help' for a list of available command-line\noptions.\n\n");
        return 1;
    } else if (HaveFilename > 1) {
        fprintf(stderr,
                "\npyxplot_fitshelper should be provided with only one filename on the command\nline to act upon. Multiple filenames appear to have been supplied. Type\n'pyxplot_fitshelper -help' for a list of available command-line options.\n\n");
        return 1;
    }

#ifndef HAVE_FITSIO
    fprintf(stderr, "\nThe cfitsio library was not installed on this computer when Pyxplot was\n\
installed, and consequently Pyxplot was compiled with no support for fits\n\
files. The fits helper application which you are using is a dummy which does\n\
nothing. To plot the datafile <%s>, you need to install libcfitsio and then\n\
recompile Pyxplot.\n\n",
            filename);
    exit(1);
#else
    // Open the FITS file
    if (fits_open_file(&fptr, filename, READONLY, &status) != 0) {
        fits_report_error(stderr, status);
        exit(1);
    }
    fits_get_hdu_num(fptr, &hdupos); // Get the current HDU number

    // Write a header describing where the text came from
    fprintf(stdout, "# Listing the contents of file <%s>\n", filename);
    fprintf(stdout, "# This ASCII representation generated by %s.\n", ppl_strStrip(version_string, warntext));
    fprintf(stdout, "# Conversion performed at %s.\n", ppl_strStrip(ppl_friendlyTimestring(), warntext));

    for (; !status; hdupos++) // Loop through extensions in this fits file
    {
        fits_get_hdrspace(fptr, &nkeys, NULL, &status); // Get number of keywords in this extension
        fprintf(stdout, "\n\n\n\n# HDU %d\n#\n", hdupos);

        for (int i = 0; i <= nkeys; i++) // List keys from this header
        {
            if (fits_read_record(fptr, i, card, &status)) break;
            fprintf(stdout, "# %s\n", card);
        }

        fits_get_hdu_type(fptr, &hdutype, &status); // Get HDU type

        // Images are not converted: just describe their size
        if (hdutype == IMAGE_HDU) {
            fits_get_img_param(fptr, 256, &bitpix, &naxis, naxes, &status);
            fprintf(stdout, "\n# Image array, with %d bits per pixel, and %d axes", abs(bitpix), naxis);
            if (naxis > 0) fprintf(stdout, ". Size: ");
            // List the size of each axis
            for (int i = 0; i < naxis; i++) {
                if (i != 0) fprintf(stdout, " x ");
                fprintf(stdout, "%ld", naxes[i]);
            }
            fprintf(stdout, ".\n\nnull\n");
        } else {
            fits_get_num_rows(fptr, &nrows, &status);
            fits_get_num_cols(fptr, &ncols, &status);

            // Tables are converted row by row, after headings listing the names and units of their columns
            if (hdutype == ASCII_TBL) fprintf(stdout, "\n# ASCII table. Size: %d x %ld.\n\n", ncols, nrows);
            else fprintf(stdout, "\n# Binary table. Size: %d x %ld.\n\n", ncols, nrows);

            // Print a list of column names
            fprintf(stdout, "# Columns: ");
            // Write the name of each column, padded to its display width
            for (int i = 1; i <= ncols; i++) {
                fits_get_col_display_width(fptr, i, &dispwidth[i], &status);
                fits_make_keyn("TTYPE", i, keyword, &status);
                fits_read_key(fptr, TSTRING, keyword, colname, NULL, &status);
                colname[dispwidth[i]] = '\0'; // Truncate long names
                printf("%*s ", dispwidth[i], colname);
            }

            // Print a list of column units
            fprintf(stdout, "\n# Units:   ");
            // Write the units of each column, padded to its display width
            for (int i = 1; i <= ncols; i++) {
                fits_get_col_display_width(fptr, i, &dispwidth[i], &status);
                fits_make_keyn("TUNIT", i, keyword, &status);
                fits_read_key(fptr, TSTRING, keyword, colname, NULL, &status);
                colname[dispwidth[i]] = '\0'; // Truncate long names
                // Columns without units are dimensionless
                if (status == KEY_NO_EXIST) {
                    status = 0;
                    strcpy(colname, "1");
                } // fits file may not specify unit
                printf("%*s ", dispwidth[i], colname);
            }
            fprintf(stdout, "\n\n");

            // Print data from table
            valptr = tabval;
            // Write each row of the table
            for (int j = 1; (j <= nrows) && (!status); j++) {
                fprintf(stdout, "           "); // Indent to meet column headings
                // Write each value in the row as a string, padded to the column's display width
                for (int i = 1; (i <= ncols) && (!status); i++) {
                    fits_read_col_str(fptr, i, j, 1, 1, nullstr, &valptr, &anynull, &status);
                    if (!status) fprintf(stdout, "%-*s ", dispwidth[i], tabval);
                }
                fprintf(stdout, "\n");
            }
        }
        fits_movrel_hdu(fptr, 1, NULL, &status); // Move to the next HDU
    }

    if (status == END_OF_FILE) status = 0; // Reset status after reading an EOF
    fits_close_file(fptr, &status);
    if (status) fits_report_error(stderr, status); // Print any error message which has caused premature halt
    return status;
#endif

    return 0; // Should never get here
}
