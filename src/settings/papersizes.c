// papersizes.c
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

// The table of named paper sizes which the "set papersize" command accepts, e.g. a4 and letter, and functions to look
// sizes up by name and to name a size from its dimensions. The ISO, Japanese and Swedish series are added to the table
// when Pyxplot starts.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "settings/papersizes.h"

typedef struct PaperSize {
    char name[32];
    double height;
    double width;
} PaperSize;

// The named paper sizes, with their heights and widths in mm. The table is completed by ppl_PaperSizeInit(), which adds
// 2A0, 4A0, and 11 sizes from each of the 9 series below, giving 27 + 2 + 99 entries and the END marker.
PaperSize PaperSizeList[129] = {{"crown", 508, 381},
                                {"demy", 572, 445},
                                {"double_demy", 889, 597},
                                {"elephant", 711, 584},
                                {"envelope_dl", 110, 220}, // Wider than it is long
                                {"executive", 267, 184},
                                {"foolscap", 330, 203},
                                {"government_letter", 267, 203},
                                {"international_businesscard", 85.60, 53.98},
                                {"large_post", 533, 419},
                                {"letter", 279, 216},
                                {"legal", 356, 216},
                                {"ledger", 432, 279},
                                {"medium", 584, 457},
                                {"monarch", 267, 184},
                                {"post", 489, 394},
                                {"quad_demy", 1143, 889},
                                {"quarto", 254, 203},
                                {"royal", 635, 508},
                                {"statement", 216, 140},
                                {"tabloid", 432, 279},
                                {"japanese_shiroku4", 379, 264},
                                {"japanese_shiroku5", 262, 189},
                                {"japanese_shiroku6", 188, 127},
                                {"japanese_kiku4", 306, 227},
                                {"japanese_kiku5", 227, 151},
                                {"us_businesscard", 89, 51},
                                {"END", 0, 0}};


//! Sizes 0-10 of each series of paper sizes, in mm (width, height). The ISO A, B and C series are as given in ISO 216
//! and ISO 269, the Japanese B series in JIS P 0138, and the Swedish D-H series in SIS 014711. The H series is not
//! defined by SIS 014711, but is the conventional extension of it. The dimensions are listed rather than computed,
//! because the standards round each size to whole millimetres.

typedef struct PaperSeries {
    const char *name;
    int size[11][2];
} PaperSeries;

static const PaperSeries PaperSeriesList[] = {
        {"a",
         {{841, 1189}, {594, 841}, {420, 594}, {297, 420}, {210, 297}, {148, 210},
          {105, 148}, {74, 105}, {52, 74}, {37, 52}, {26, 37}}},
        {"b",
         {{1000, 1414}, {707, 1000}, {500, 707}, {353, 500}, {250, 353}, {176, 250},
          {125, 176}, {88, 125}, {62, 88}, {44, 62}, {31, 44}}},
        {"c",
         {{917, 1297}, {648, 917}, {458, 648}, {324, 458}, {229, 324}, {162, 229},
          {114, 162}, {81, 114}, {57, 81}, {40, 57}, {28, 40}}},
        {"japanese_b",
         {{1030, 1456}, {728, 1030}, {515, 728}, {364, 515}, {257, 364}, {182, 257},
          {128, 182}, {91, 128}, {64, 91}, {45, 64}, {32, 45}}},
        {"swedish_d",
         {{1091, 1542}, {771, 1091}, {545, 771}, {386, 545}, {273, 386}, {193, 273},
          {136, 193}, {96, 136}, {68, 96}, {48, 68}, {34, 48}}},
        {"swedish_e",
         {{878, 1242}, {621, 878}, {439, 621}, {310, 439}, {220, 310}, {155, 220},
          {110, 155}, {78, 110}, {55, 78}, {39, 55}, {27, 39}}},
        {"swedish_f",
         {{1044, 1477}, {738, 1044}, {522, 738}, {369, 522}, {261, 369}, {185, 261},
          {131, 185}, {92, 131}, {65, 92}, {46, 65}, {33, 46}}},
        {"swedish_g",
         {{958, 1354}, {677, 958}, {479, 677}, {339, 479}, {239, 339}, {169, 239},
          {120, 169}, {85, 120}, {60, 85}, {42, 60}, {30, 42}}},
        {"swedish_h",
         {{1139, 1610}, {805, 1139}, {569, 805}, {403, 569}, {285, 403}, {201, 285},
          {142, 201}, {101, 142}, {71, 101}, {50, 71}, {36, 50}}},
        {NULL, {{0, 0}}}};

//! ppl_PaperSizeInit - Populate the table of named paper sizes, including the ISO A, B and C series and the Swedish
//! extensions to them

void ppl_PaperSizeInit() {
    int pos;

    // Find the end of the fixed part of the table, where the computed entries begin
    for (pos = 0; (strcmp(PaperSizeList[pos].name, "END") != 0); pos++);

    // 4A0 is four times the size of A0
    strcpy(PaperSizeList[pos].name, "4A0");
    PaperSizeList[pos].width = 1682;
    PaperSizeList[pos].height = 2378;
    pos++;

    // 2A0 is twice the size of A0
    strcpy(PaperSizeList[pos].name, "2A0");
    PaperSizeList[pos].width = 1189;
    PaperSizeList[pos].height = 1682;
    pos++;

    // Add sizes 0-10 of each series, e.g. a0 to a10
    for (int j = 0; PaperSeriesList[j].name != NULL; j++)
        // Name each size by its series and number, e.g. b5
        for (int i = 0; i < 11; i++) {
            ppl_snprintfTrunc(PaperSizeList[pos].name, sizeof(PaperSizeList[pos].name), "%s%d",
                              PaperSeriesList[j].name, i);
            PaperSizeList[pos].width = PaperSeriesList[j].size[i][0];
            PaperSizeList[pos].height = PaperSeriesList[j].size[i][1];
            pos++;
        }
    strcpy(PaperSizeList[pos].name, "END"); // Marker of end of papersize list
}

//! ppl_PaperSizeByName - Look up the dimensions of a named paper size
//! \param [in] name - The name of the paper size, e.g. "a4"
//! \param [out] height - Set to the height of the paper, in mm, or zero if the paper size is not recognised
//! \param [out] width - Set to the width of the paper, in mm, or zero if the paper size is not recognised

void ppl_PaperSizeByName(const char *name, double *height, double *width) {
    int i = 0;
    // Search the table for the name, case-insensitively
    while (1) {
        if (strcmp(PaperSizeList[i].name, "END") == 0) // The requested papersize doesn't exist
        {
            *height = 0.0;
            *width = 0.0;
            return;
        }
        if (ppl_strCmpNoCase(PaperSizeList[i].name, name) == 0) // We've found the requested papersize
        {
            *height = PaperSizeList[i].height;
            *width = PaperSizeList[i].width;
            return;
        }
        i++;
    }
}

//! ppl_GetPaperName - Look up the name of the paper size with specified dimensions
//! \param [out] name - Character buffer into which to write the name of the paper size, or "User-defined
//! papersize" if it does not match any named paper size
//! \param [in] height - The height of the paper, in mm
//! \param [in] width - The width of the paper, in mm

void ppl_GetPaperName(char *name, const double *height, const double *width) {
    int i = 0;
    // Search the table for a size within half a millimetre of the given dimensions
    while (1) {
        if (strcmp(PaperSizeList[i].name, "END") == 0) // The requested papersize doesn't exist
        {
            strcpy(name, "User-defined papersize");
            return;
        }
        if ((fabs(PaperSizeList[i].height - *height) < 0.5) &&
            (fabs(PaperSizeList[i].width - *width) < 0.5)) // We've found the requested papersize
        {
            strcpy(name, PaperSizeList[i].name);
            return;
        }
        i++;
    }
}
