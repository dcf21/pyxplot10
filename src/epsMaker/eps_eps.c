// eps_eps.c
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

// Draws eps canvas items, made with the eps command, which include an existing encapsulated postscript file in the
// output. The file's bounding box is read from its %%BoundingBox comments, or computed by running it through
// ghostscript's bbox device, and is used to scale and position the file; its contents are then copied into the
// postscript buffer between BeginEPSF and EndEPSF, during the final rendering pass of canvasDraw.c.

#define _PPL_EPS_EPS 1

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <glob.h>
#include <wordexp.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"

#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_eps.h"
#include "epsMaker/eps_settings.h"

//! eps_eps_RenderEPS - Render an eps canvas item, which embeds an encapsulated postscript file, to postscript
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_eps_RenderEPS(EPSComm *x) {
    // Variables for the file and its placement
    char msg[PPL_MSG_LENGTH];
    FILE *inf;
    char filename[FNAME_LENGTH];
    double bb_left = 0.0, bb_right = 0.0, bb_top = 0.0, bb_bottom = 0.0;
    double xscale, yscale, r;
    double xanchor = 0, yanchor = 0, xbotleft, ybotleft;
    unsigned char GotBBox;

    // Label the item in the postscript, and expand the filename
    fprintf(x->epsbuffer, "%% Canvas item %d [eps image]\n", x->current->id);

    // Expand filename if it contains wildcards
    {
        wordexp_t wordExp;
        glob_t globData;
        char fName[FNAME_LENGTH];
        {
            int j, k;
            // Escape spaces in the filename before passing it to wordexp
            for (j = k = 0; ((x->current->text[j] != '\0') && (k < FNAME_LENGTH - 1));) {
                if (x->current->text[j] == ' ') fName[k++] = '\\';
                fName[k++] = x->current->text[j++];
            }
            fName[k++] = '\0';
        }
        // Expand ~ and environment variables in the filename
        if ((wordexp(fName, &wordExp, 0) != 0) || (wordExp.we_wordc <= 0)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Could not open file '%s'.", fName);
            ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
            return;
        }
        // Expand wildcards in the filename
        if ((glob(wordExp.we_wordv[0], 0, NULL, &globData) != 0) || (globData.gl_pathc <= 0)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Could not open file '%s'.", fName);
            ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
            wordfree(&wordExp);
            return;
        }
        wordfree(&wordExp);
        snprintf(filename, FNAME_LENGTH, "%s", globData.gl_pathv[0]);
        filename[FNAME_LENGTH - 1] = '\0';
        globfree(&globData);
    }

    // Work out bounding box of EPS image
    GotBBox = 0;
    if (!x->current->calcbbox) // Read bounding box from headers of EPS file
    {
        inf = fopen(filename, "r");
        // Report a failure to open the file
        if (inf == NULL) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Could not open EPS file '%s'.", filename);
            ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
            *(x->status) = 1;
            return;
        }
        eps_eps_ExtractBBox(x, inf, &bb_left, &bb_bottom, &bb_right, &bb_top, &GotBBox);
        fclose(inf);
        // Warn that the bounding box will have to be computed
        if (!GotBBox) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Could not extract bounding box from EPS file '%s'. Will therefore process file in calcbbox mode, and attempt to determine its bounding box using ghostview.",
                    filename);
            ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
        }
    }
    if ((x->current->calcbbox) || (!GotBBox)) // Calculate bounding box for EPS file using ghostview
    {
        char command[LSTR_LENGTH], tmpdata[FNAME_LENGTH];
        // Temporary file for gs to output bounding box into
        if ((snprintf(tmpdata, FNAME_LENGTH, "%s%s%s", x->c->errcontext.session_default.tempdir, PATHLINK, "bbox_in") >=
             FNAME_LENGTH) ||
            (snprintf(command, LSTR_LENGTH, "%s -dQUIET -dSAFER -dBATCH -dNOPAUSE -sDEVICE=bbox %s > %s 2> %s",
                      GHOSTSCRIPT_COMMAND, filename, tmpdata, tmpdata) >= LSTR_LENGTH)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Filename of EPS file '%s' is too long to calculate its bounding box.",
                              filename);
            ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
            *(x->status) = 1;
            return;
        }
        if (system(command))
            if (DEBUG) { ppl_log(&x->c->errcontext, "Ghostscript returned non-zero return value."); }
        inf = fopen(tmpdata, "r");
        // Report a failure to open ghostscript's output
        if (inf == NULL) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Could not open temporary file '%s'.", tmpdata);
            ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
            *(x->status) = 1;
            return;
        }
        eps_eps_ExtractBBox(x, inf, &bb_left, &bb_bottom, &bb_right, &bb_top, &GotBBox);
        fclose(inf);
        // Warn that the bounding box could not be found
        if (!GotBBox) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Could not calculate bounding box for EPS file '%s'.", filename);
            ppl_warning(&x->c->errcontext, ERR_GENERIC, msg);
        }
    }

    // Work out scaling factor to apply to EPS image
    if (GotBBox && (x->current->xpos2set) && (x->current->ypos2set)) // Both width and height have been specified
    {
        xscale = x->current->xpos2 * M_TO_PS / (bb_right - bb_left);
        yscale = x->current->ypos2 * M_TO_PS / (bb_top - bb_bottom);
    } else if (GotBBox && x->current->xpos2set) // Only width has been set
    {
        xscale = x->current->xpos2 * M_TO_PS / (bb_right - bb_left);
        yscale = x->current->xpos2 * M_TO_PS / (bb_right - bb_left);
    } else if (GotBBox && x->current->ypos2set) // Only height has been set
    {
        xscale = x->current->ypos2 * M_TO_PS / (bb_top - bb_bottom);
        yscale = x->current->ypos2 * M_TO_PS / (bb_top - bb_bottom);
    } else // Neither height nor width has been set; EPS file should be drawn at its natural size
    {
        xscale = 1.0;
        yscale = 1.0;
    }

    // Work out position of bottom-left corner
    r = x->current->rotation;
    if (x->current->settings.TextHAlign == SW_HALIGN_LEFT) xanchor = 0;
    else if (x->current->settings.TextHAlign == SW_HALIGN_CENT) xanchor = (xscale) / 2;
    else if (x->current->settings.TextHAlign == SW_HALIGN_RIGHT) xanchor = (xscale);
    if (x->current->settings.TextVAlign == SW_VALIGN_TOP) yanchor = (yscale);
    else if (x->current->settings.TextVAlign == SW_VALIGN_CENT) yanchor = (yscale) / 2;
    else if (x->current->settings.TextVAlign == SW_VALIGN_BOT) yanchor = 0;
    xanchor *= (bb_right - bb_left);
    yanchor *= (bb_top - bb_bottom);
    xbotleft = x->current->xpos * M_TO_PS - (xanchor * cos(r) - yanchor * sin(r));
    ybotleft = x->current->ypos * M_TO_PS - (xanchor * sin(r) + yanchor * cos(r));

    // Begin encapsulation of EPS file
    fprintf(x->epsbuffer, "BeginEPSF\n");
    fprintf(x->epsbuffer, "%.2f %.2f translate %% Position the EPS file\n", xbotleft, ybotleft);
    fprintf(x->epsbuffer, "%.2f rotate         %% Rotation of EPS graphic\n", x->current->rotation * 180 / M_PI);
    fprintf(x->epsbuffer, "%f %f scale         %% Scale to desired size\n", xscale, yscale);
    fprintf(x->epsbuffer, "%f %f translate     %% Move to lower left of the EPS\n", -bb_left, -bb_bottom);

    // Clip EPS to bounding box if requested
    if ((x->current->clip) && (GotBBox)) {
        fprintf(x->epsbuffer, "newpath\n%f %f moveto %f %f lineto %f %f lineto %f %f lineto closepath\n", bb_left,
                bb_bottom, bb_right, bb_bottom, bb_right, bb_top, bb_left, bb_top);
        fprintf(x->epsbuffer, "clip newpath %% Set clipping path around bounding box\n");
    }

    // Copy contents of EPS into output postscript file
    fprintf(x->epsbuffer, "%% ---- Beginning of included EPS graphic ----\n%%%%BeginDocument: epsfile.eps\n");
    inf = fopen(filename, "r");
    // Report a failure to open the file
    if (inf == NULL) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Could not open EPS file '%s'.", filename);
        ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
        *(x->status) = 1;
        return;
    }
    while (fgets(msg, FNAME_LENGTH, inf) != NULL)
        // Report a failure to copy the file
        if (fputs(msg, x->epsbuffer) == EOF) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Error while reading EPS file '%s'.", filename);
            ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
            *(x->status) = 1;
            fclose(inf);
            return;
        }
    fclose(inf);

    // Finish off encapsulation of EPS file
    fprintf(x->epsbuffer, "%%%%EOF\n%%%%EndDocument\n\n%% ---- End of included EPS graphic ----\nEndEPSF\n");

    // Update postscript bounding box
    xscale *= (bb_right -
               bb_left); // Multiply scaling factors by width and height to give total size of resulting image in points
    yscale *= (bb_top - bb_bottom);
    eps_core_BoundingBox(x, xbotleft, ybotleft, 0);
    eps_core_BoundingBox(x, xbotleft + xscale * cos(r), ybotleft + xscale * sin(r), 0);
    eps_core_BoundingBox(x, xbotleft + yscale * -sin(r), ybotleft + yscale * cos(r), 0);
    eps_core_BoundingBox(x, xbotleft + xscale * cos(r) + yscale * -sin(r), ybotleft + xscale * sin(r) + yscale * cos(r),
                         0);

    // Final newline at end of canvas item
    fprintf(x->epsbuffer, "\n");
    return;
}

//! readBBox - Read the four numbers which follow a %%BoundingBox: or %%HiResBoundingBox: comment
//! \param [in] msg - The remainder of the line after the comment's name
//! \param [out] box - The four numbers read: left, bottom, right and top
//! \return One if four numbers were read, or zero if the bounding box was malformed

static int readBBox(const char *msg, double *box) {
    int i = 0;
    for (int k = 0; k < 4; k++) {
        int j;
        while (((unsigned char) msg[i] <= ' ') && (msg[i] != '\0')) i++;
        box[k] = ppl_getFloat(msg + i, &j);
        if (j <= 0) return 0;
        i += j;
    }
    return 1;
}

//! eps_eps_ExtractBBox - Read the bounding box of an EPS file from its %%BoundingBox (or %%HiResBoundingBox) comment
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] in - The file handle of the EPS file
//! \param [out] bl - Set to the left edge of the bounding box
//! \param [out] bb - Set to the bottom edge of the bounding box
//! \param [out] br - Set to the right edge of the bounding box
//! \param [out] bt - Set to the top edge of the bounding box
//! \param [out] GotBox - Set to one if a bounding box was found, zero otherwise

void eps_eps_ExtractBBox(const EPSComm *x, FILE *in, double *bl, double *bb, double *br, double *bt,
                         unsigned char *GotBox) {
    const char *const boxStr[2] = {"%%BoundingBox:", "%%HiResBoundingBox:"};
    int matched[2] = {0, 0}, j;
    *GotBox = 0;

    // Search the file for the two comments, matching each one character by character as the file is read
    while ((j = fgetc(in)) != EOF) {
        for (int k = 0; k < 2; k++) {
            char msg[PPL_MSG_LENGTH];
            double box[4];
            // Advance through the comment's name while the characters match
            matched[k] = (boxStr[k][matched[k]] == j) ? (matched[k] + 1) : 0;
            if (boxStr[k][matched[k]] != '\0') continue;
            // After the comment's name, read four numbers
            matched[k] = 0;
            if ((fgets(msg, FNAME_LENGTH, in) == NULL) || !readBBox(msg, box)) continue;
            *bl = box[0];
            *bb = box[1];
            *br = box[2];
            *bt = box[3];
            *GotBox = 1;
            // A high resolution bounding box takes precedence, so we've got all we need. Otherwise, keep searching in
            // case there is also a high resolution bounding box.
            if (k == 1) return;
        }
    }
}
