// canvasDraw.c
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

#define _CANVASDRAW_C 1

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <wordexp.h>
#include <sys/select.h>
#include <sys/wait.h>

#include <gsl/gsl_math.h>

#include "coreUtils/backup.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"
#include "coreUtils/memAlloc.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "epsMaker/canvasDraw.h"
#include "epsMaker/dvi_read.h"
#include "epsMaker/eps_arrow.h"
#include "epsMaker/eps_box.h"
#include "epsMaker/eps_circle.h"
#include "epsMaker/eps_ellipse.h"
#include "epsMaker/eps_eps.h"
#include "epsMaker/eps_image.h"
#include "epsMaker/eps_piechart.h"
#include "epsMaker/eps_plot.h"
#include "epsMaker/eps_plot_threedimbuff.h"
#include "epsMaker/eps_point.h"
#include "epsMaker/eps_polygon.h"
#include "epsMaker/eps_text.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_settings.h"
#include "userspace/context.h"
#include "canvasItems.h"
#include "children.h"
#include "datafile.h"

//! filecopy - Copy a file
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] in - The path of the file to copy
//! \param [in] out - The path to copy it to
//! \return Zero on success, or one on failure

static int filecopy(EPSComm *x, const char *in, const char *out) {
    char msg[PPL_MSG_LENGTH];
    int status = 0, n;
    FILE *fi, *fo;
    ppl_createBackupIfRequired(x->c, out);
    fi = fopen(in, "rb");
    fo = fopen(out, "wb");
    if ((fo == NULL) || (fi == NULL)) status = 1;
    while ((!status) && ((n = fread((void *) msg, 1, FNAME_LENGTH, fi)) > 0))
        if (fwrite((void *) msg, 1, n, fo) < n) status = 1;
    if (fi != NULL) fclose(fi);
    if (fo != NULL) fclose(fo);
    return status;
}

// Run ghostscript to convert postscript output into bitmap graphics
#define BITMAP_TERMINAL_CLEANUP(X, Y, CanInvert) \
    if (system(msg) != 0) /* Run ghostscript */ \
    { \
        ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, \
                  "Error encountered whilst using " X " to generate " Y " output"); \
    } else { \
        char *src = GSOutputTemp; \
        if ((CanInvert) && (c->set->term_current.TermInvert == SW_ONOFF_ON)) { \
            ppl_snprintfTrunc(msg, sizeof(msg), "%s %s -negate %s", CONVERT_COMMAND, GSOutputTemp, GSOutputTemp2); \
            if (system(msg) != 0) /* Run convert to negate image */ \
            { \
                ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, \
                          "Error encountered whilst using ImageMagick to generate negated " Y " output"); \
            } \
            src = GSOutputTemp2; \
            remove(GSOutputTemp); \
        } \
        if (filecopy(&comm, src, comm.FinalFilename) != 0) /* Move ghostscript output to desire target file */ \
        { \
            ppl_snprintfTrunc(msg, sizeof(msg), "Could not write output to file '%s'", comm.FinalFilename); \
            ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg); \
        } \
        remove(src); /* Delete temporary files */ \
    } \
    remove(comm.EPSFilename);

// Standard postscript prolog text for EPS and PS files
static char *EPS_PROLOG_TEXT = "\
/BeginEPSF { %def                                     % Prepare for EPS file\n\
 /b4_Inc_state save def                               % Save state for cleanup\n\
 /dict_count countdictstack def                       % Count objects on dict stack\n\
 /op_count count 1 sub def                            % Count objects on operand stack\n\
 userdict begin                                       % Push userdict on dict stack\n\
 /showpage { } def                                    % Redefine showpage, { } = null proc\n\
 0 setgray 0 setlinecap 1 setlinewidth 0 setlinejoin  % Prepare graphics state\n\
 10 setmiterlimit [ ] 0 setdash newpath\n\
 /languagelevel where                                 % If level not equal to 1 then set strokeadjust and overprint to their defaults.\n\
  {pop languagelevel\n\
   1 ne\n\
    {false setstrokeadjust false setoverprint\n\
    } if\n\
  } if\n\
} bind def\n\
\n\
/EndEPSF { %def                                       % Clean up after including an EPS file\n\
 count op_count sub {pop} repeat                      % Clean up stacks\n\
 countdictstack dict_count sub {end} repeat\n\
 b4_Inc_state restore\n\
} bind def\n\
";
static char *PS_PROLOG_TEXT = "\
";

// Standard command-line flags to pass to ghostscript
static char *GHOSTSCRIPT_STANDARD_FLAGS = "-dQUIET -dSAFER -P- -dBATCH -dNOPAUSE -dEPSCrop";

// Table of the functions we call for each phase of the canvas drawing process for different object types
static void (*ArrowHandlers[])(EPSComm *) = {NULL, NULL, NULL, NULL, NULL, eps_arrow_RenderEPS, NULL};
static void (*BoxHandlers[])(EPSComm *) = {NULL, NULL, NULL, NULL, NULL, eps_box_RenderEPS, NULL};
static void (*CircHandlers[])(EPSComm *) = {NULL, NULL, NULL, NULL, NULL, eps_circ_RenderEPS, NULL};
static void (*EllpsHandlers[])(EPSComm *) = {NULL, NULL, NULL, NULL, NULL, eps_ellps_RenderEPS, NULL};
static void (*EPSHandlers[])(EPSComm *) = {NULL, NULL, NULL, NULL, NULL, eps_eps_RenderEPS, NULL};
static void (*ImageHandlers[])(EPSComm *) = {NULL, NULL, NULL, NULL, NULL, eps_image_RenderEPS, NULL};
static void (*PieHandlers[])(EPSComm *) = {eps_pie_ReadAccessibleData, NULL, NULL, eps_pie_YieldUpText, NULL,
                                           eps_pie_RenderEPS,          NULL};
static void (*PlotHandlers[])(EPSComm *) = {eps_plot_ReadAccessibleData,
                                            eps_plot_SampleFunctions,
                                            eps_plot_DecideAxisRanges,
                                            eps_plot_YieldUpText,
                                            NULL,
                                            eps_plot_RenderEPS,
                                            NULL};
static void (*PointHandlers[])(EPSComm *) = {NULL, NULL, NULL, eps_point_YieldUpText, NULL, eps_point_RenderEPS, NULL};
static void (*PolygHandlers[])(EPSComm *) = {NULL, NULL, NULL, NULL, NULL, eps_polygon_RenderEPS, NULL};
static void (*TextHandlers[])(EPSComm *) = {NULL, NULL, NULL, eps_text_YieldUpText, NULL, eps_text_RenderEPS, NULL};
static void (*AfterHandlers[])(EPSComm *) = {NULL, NULL, NULL, canvas_CallLaTeX, canvas_MakeEPSBuffer, canvas_EPSWrite,
                                             NULL};


//! ppl_canvas_draw - Render the multiplot canvas to the current output terminal. Each canvas item is rendered in
//! several passes: data are read, text is collected and typeset with LaTeX, and then postscript is written, before
//! being converted to the output format if required.
//! \param [in] c - The Pyxplot context
//! \param [out] unsuccessful_ops - Array of flags, one per canvas item ID, set to one for each item which could not be
//! rendered
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_canvas_draw(ppl_context *c, unsigned char *unsuccessful_ops, const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    static int lock = 0;
    int i, j, termtype, status = 0, CSPCommand = 0;
    static long TempFile_counter = 0, TeXFile_counter = 0;
    char EPSFilenameTemp[FNAME_LENGTH], TeXFilenameTemp[FNAME_LENGTH], TitleTemp[FNAME_LENGTH],
            FinalFilenameTemp[FNAME_LENGTH];
    char GSOutputTemp[FNAME_LENGTH], GSOutputTemp2[FNAME_LENGTH];
    wordexp_t WordExp;
    char *EnvDisplay;
    EPSComm comm;
    canvas_item *item;
    void (*ArrowHandler)(EPSComm *);
    void (*BoxHandler)(EPSComm *);
    void (*CircHandler)(EPSComm *);
    void (*EllpsHandler)(EPSComm *);
    void (*EPSHandler)(EPSComm *);
    void (*ImageHandler)(EPSComm *);
    void (*PieHandler)(EPSComm *);
    void (*PlotHandler)(EPSComm *);
    void (*PointHandler)(EPSComm *);
    void (*PolygHandler)(EPSComm *);
    void (*TextHandler)(EPSComm *);
    void (*AfterHandler)(EPSComm *);

    // Try to get lock
    if (__sync_add_and_fetch(&lock, 1) > 1) {
        ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, "Recursive call to the plot command detected.");
        return;
    }

    // Reset 3D rendering buffer
    comm.iterDepth = iterDepth;
    comm.c = c;
    ThreeDimBuffer_Reset(&comm);

    // By default, we record all operations as having been successful
    for (i = 0; i < MULTIPLOT_MAXINDEX; i++) unsuccessful_ops[i] = 0;

    // Work out filename to save output postscript to
    comm.termtype = termtype = c->set->term_current.TermType;
    comm.FinalFilename =
            c->set->term_current.output; // The final filename of whatever kind of output we're planning to produce
    if ((comm.FinalFilename == NULL) || (comm.FinalFilename[0] == '\0')) {
        comm.FinalFilename = FinalFilenameTemp; // If final target filename is blank, use pyxplot.<filetype>
        ppl_snprintfTrunc(FinalFilenameTemp, sizeof(FinalFilenameTemp), "pyxplot.%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, termtype, SW_TERMTYPE_INT,
                                                          (void *) SW_TERMTYPE_STR, sizeof(char *)));
    }

    // Perform expansion of shell filename shortcuts such as ~
    if ((wordexp(comm.FinalFilename, &WordExp, 0) != 0) || (WordExp.we_wordc <= 0)) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Could not find directory containing filename '%s'.", comm.FinalFilename);
        ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg);
        lock = 0;
        return;
    }
    if (WordExp.we_wordc > 1) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Filename '%s' is ambiguous.", comm.FinalFilename);
        ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg);
        lock = 0;
        return;
    }
    ppl_snprintfTrunc(FinalFilenameTemp, sizeof(FinalFilenameTemp), "%s", WordExp.we_wordv[0]);
    wordfree(&WordExp);
    comm.FinalFilename = FinalFilenameTemp;

    // Create filename for temporary LaTeX document
    comm.TeXFilename = TeXFilenameTemp;
    TeXFile_counter++;
    if (snprintf(TeXFilenameTemp, FNAME_LENGTH, "%s%spyxplot_%d_%ld", c->errcontext.session_default.tempdir, PATHLINK,
                 getpid(), TempFile_counter) >= FNAME_LENGTH)
        ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Path of temporary directory is too long.");

    // Case 1: EPS and PS terminals. Postscript output will be saved immediately in situ.
    if ((termtype == SW_TERMTYPE_EPS) || (termtype == SW_TERMTYPE_PS)) {
        comm.EPSFilename =
                comm.FinalFilename; // eps or ps is actually what we're aiming to make, so save straight to user's chosen target
        ppl_createBackupIfRequired(c, comm.EPSFilename);
        comm.title = comm.EPSFilename; // Filename 'foo/bar/myplot.eps' --> title 'myplot.eps'
        for (i = 0; comm.title[i] != '\0'; i++)
            if ((comm.title[i] == PATHLINK[0]) && ((i == 0) || (comm.title[i - 1] != '\\'))) {
                comm.title += i + 1;
                i = -1;
            }
    } else // Case 2: All other terminals. Create a temporary eps file for subsequent processing.
    {
        comm.EPSFilename = EPSFilenameTemp;
        TempFile_counter++;
        if (snprintf(EPSFilenameTemp, FNAME_LENGTH, "%s%spyxplot_%d_%ld.eps", c->errcontext.session_default.tempdir,
                     PATHLINK, getpid(), TempFile_counter) >= FNAME_LENGTH)
            ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Path of temporary directory is too long.");
        if ((termtype == SW_TERMTYPE_X11S) || (termtype == SW_TERMTYPE_X11M) || (termtype == SW_TERMTYPE_X11P)) {
            comm.title = TitleTemp; // In X11 terminals, we give eps files numbered titles
            ppl_snprintfTrunc(TitleTemp, sizeof(TitleTemp), "Pyxplot Output %ld", TempFile_counter);
        } else {
            comm.title =
                    comm.FinalFilename; // In other terminals, we give eps files filename titles, with path cut off front
            for (i = 0; comm.title[i] != '\0'; i++)
                if ((comm.title[i] == PATHLINK[0]) && ((i == 0) || (comm.title[i - 1] != '\\'))) {
                    comm.title += i;
                    i = 0;
                }
        }
    }

    // Make GSOutputTemp a temporary file that ghostscript can spit data out to (with a shell-safe filename)
    if ((snprintf(GSOutputTemp, FNAME_LENGTH, "%s%spyxplot_%d_%ld%s", c->errcontext.session_default.tempdir, PATHLINK,
                  getpid(), TempFile_counter, (termtype == SW_TERMTYPE_GIF) ? ".gif" : ".tmp") >= FNAME_LENGTH) ||
        (snprintf(GSOutputTemp2, FNAME_LENGTH, "%s%spyxplot_%d_%ld_%s", c->errcontext.session_default.tempdir, PATHLINK,
                  getpid(), TempFile_counter, (termtype == SW_TERMTYPE_GIF) ? ".gif" : ".tmp") >= FNAME_LENGTH))
        ppl_fatal(&c->errcontext, __FILE__, __LINE__, "Path of temporary directory is too long.");

    // Set up communications data structure for objects we are rendering
    comm.itemlist = (canvas_itemlist *) c->canvas_items;
    comm.bb_left = comm.bb_right = comm.bb_top = comm.bb_bottom = 0.0;
    comm.bb_set = 0;
    comm.epsbuffer = NULL;
    comm.status = &status;
    comm.NTextItems = 0; // Used to count items as we add them to text buffer
    comm.TextItems = ppl_listInit(0); // Empty list of pieces of text we are going to put on canvas
    comm.LastPSColor[0] = '\0';
    comm.CurrentColor[0] = '\0';
    comm.CurrentFillColor[0] = '\0';
    comm.LastLinewidth = -1.0;
    comm.LastLinetype = 0;
    comm.LaTeXpageno = 0; // Used to count items off as we render them to postscript
    comm.dvi = NULL;
    for (i = 0; i < N_POINTTYPES; i++)
        comm.PointTypesUsed[i] =
                0; // Record which point and star macros we've used and need to include in postscript prolog
    for (i = 0; i < N_STARTYPES; i++) comm.StarTypesUsed[i] = 0;

    // Loop over all of the items on the canvas zeroing the plot bounding boxes
    if (comm.itemlist != NULL)
        for (item = comm.itemlist->first; item != NULL; item = item->next) {
            item->PlotLeftMargin = 0.0;
            item->PlotRightMargin = 0.0;
            item->PlotTopMargin = 0.0;
            item->PlotBottomMargin = 0.0;
        }

    // Rendering of EPS occurs in a series of phases which we now loop over
    for (j = 0;; j++) {
        ArrowHandler = ArrowHandlers[j]; // Each object type has a handler for each phase of postscript generation
        BoxHandler = BoxHandlers[j];
        CircHandler = CircHandlers[j];
        EllpsHandler = EllpsHandlers[j];
        EPSHandler = EPSHandlers[j];
        ImageHandler = ImageHandlers[j];
        PlotHandler = PlotHandlers[j];
        PieHandler = PieHandlers[j];
        PointHandler = PointHandlers[j];
        PolygHandler = PolygHandlers[j];
        TextHandler = TextHandlers[j];
        AfterHandler = AfterHandlers[j];
        if ((j > 4) && (ArrowHandler == NULL) && (CircHandler == NULL) && (EllpsHandler == NULL) &&
            (EPSHandler == NULL) && (ImageHandler == NULL) && (PieHandler == NULL) && (PlotHandler == NULL) &&
            (PointHandler == NULL) && (PolygHandler == NULL) && (TextHandler == NULL) && (AfterHandler == NULL))
            break;

        // Loop over all of the items on the canvas
        if (comm.itemlist != NULL)
            for (item = comm.itemlist->first; item != NULL; item = item->next) {
                if (item->deleted) continue; // ... except those which have been deleted
                if (unsuccessful_ops[item->id]) continue; // ... or which have already failed
                comm.LastPSColor[0] = '\0'; // Make each item produce free-standing postscript for easy editing
                comm.CurrentColor[0] = '\0';
                comm.CurrentFillColor[0] = '\0';
                comm.LastLinewidth = -1.0;
                comm.LastLinetype = 0;
                comm.current = item;
                if ((item->type == CANVAS_ARROW) && (ArrowHandler != NULL))
                    (*ArrowHandler)(&comm); // Call the relevant handler for each one
                else if ((item->type == CANVAS_BOX) && (BoxHandler != NULL)) (*BoxHandler)(&comm);
                else if ((item->type == CANVAS_CIRC) && (CircHandler != NULL)) (*CircHandler)(&comm);
                else if ((item->type == CANVAS_ELLPS) && (EllpsHandler != NULL)) (*EllpsHandler)(&comm);
                else if ((item->type == CANVAS_EPS) && (EPSHandler != NULL)) (*EPSHandler)(&comm);
                else if ((item->type == CANVAS_IMAGE) && (ImageHandler != NULL)) (*ImageHandler)(&comm);
                else if ((item->type == CANVAS_PIE) && (PieHandler != NULL)) (*PieHandler)(&comm);
                else if ((item->type == CANVAS_PLOT) && (PlotHandler != NULL)) (*PlotHandler)(&comm);
                else if ((item->type == CANVAS_POINT) && (PointHandler != NULL)) (*PointHandler)(&comm);
                else if ((item->type == CANVAS_POLYGON) && (PolygHandler != NULL)) (*PolygHandler)(&comm);
                else if ((item->type == CANVAS_TEXT) && (TextHandler != NULL)) (*TextHandler)(&comm);
                if (status) {
                    unsuccessful_ops[item->id] = 1;
                } // If something went wrong... flag it up and give up on this object
                status = 0;
            }
        if (AfterHandler != NULL)
            (*AfterHandler)(&comm); // At the end of each phase, a canvas-wide handler may be called
        if (status) {
            if (comm.epsbuffer != NULL) fclose(comm.epsbuffer);
            lock = 0;
            return;
        } // The failure of a canvas-wide handler is fatal
    }

    // Now convert eps output to bitmaped graphics if requested
    if ((termtype == SW_TERMTYPE_X11S) || (termtype == SW_TERMTYPE_X11M) ||
        (termtype == SW_TERMTYPE_X11P)) // X11 terminals
    {
        EnvDisplay = getenv("DISPLAY"); // Check whether the environment variable DISPLAY is set
        if (c->set->term_current.viewer == SW_VIEWER_NULL) {
            ppl_error(
                    &c->errcontext, ERR_GENERIC, -1, -1,
                    "An attempt is being made to use an X11 terminal for output, but the required package 'ghostview' could not be found when Pyxplot was installed. If you have recently install ghostview, please reconfigure and recompile Pyxplot.");
        } else if ((EnvDisplay == NULL) || (EnvDisplay[0] == '\0')) {
            ppl_error(
                    &c->errcontext, ERR_GENERIC, -1, -1,
                    "An attempt is being made to use an X11 terminal for output, but your DISPLAY environment variable is not set; there is no accessible X11 display.");
        } else {
            if (!c->willBeInteractive) {
                if (termtype != SW_TERMTYPE_X11P) {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "An attempt is being made to use the %s terminal in a non-interactive Pyxplot session. This won't work, as the window will close as soon as Pyxplot exits. Reverting to the X11_persist terminal instead.",
                            *(char **) ppl_fetchSettingName(&c->errcontext, termtype, SW_TERMTYPE_INT,
                                                            (void *) SW_TERMTYPE_STR, sizeof(char *)));
                    ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, msg);
                }
                CSPCommand = 2;
            } else {
                if (termtype == SW_TERMTYPE_X11S) CSPCommand = 0;
                else if (termtype == SW_TERMTYPE_X11M) CSPCommand = 1;
                else if (termtype == SW_TERMTYPE_X11P) CSPCommand = 2;
            }
            if (c->set->term_current.viewer == SW_VIEWER_GV)
                ppl_snprintfTrunc(msg, sizeof(msg), "%d%s %swatch %s\n", CSPCommand, GHOSTVIEW_COMMAND, GHOSTVIEW_OPT,
                                  comm.EPSFilename);
            else if (c->set->term_current.viewer == SW_VIEWER_GGV)
                ppl_snprintfTrunc(msg, sizeof(msg), "%d%s --watch %s\n", CSPCommand, GGV_COMMAND, comm.EPSFilename);
            else
                ppl_snprintfTrunc(msg, sizeof(msg), "%d%s %s\n", CSPCommand, c->set->term_current.ViewerCmd,
                                  comm.EPSFilename);
            pplcsp_sendCommand(c, msg);

            // Pause after launching an X11 ghostview viewer to give stderr error messages a moment to come back to us
            for (i = 0; i < 7; i++) {
                struct timespec waitperiod, waitedperiod;
                waitperiod.tv_sec = 0;
                waitperiod.tv_nsec = 40000000;
                nanosleep(&waitperiod, &waitedperiod);
            }
        }
    } else if (termtype == SW_TERMTYPE_PDF) // PDF terminal
    {
        ppl_snprintfTrunc(msg, sizeof(msg), "%s %s -sDEVICE=pdfwrite -sOutputFile=%s %s", GHOSTSCRIPT_COMMAND,
                          GHOSTSCRIPT_STANDARD_FLAGS, GSOutputTemp, comm.EPSFilename);
        BITMAP_TERMINAL_CLEANUP("ghostscript", "pdf", 0);
    } else if (termtype == SW_TERMTYPE_PNG) // PNG terminal
    {
        ppl_snprintfTrunc(msg, sizeof(msg), "%s %s -sDEVICE=%s -r%d %s -sOutputFile=%s %s", GHOSTSCRIPT_COMMAND,
                          GHOSTSCRIPT_STANDARD_FLAGS,
                          (c->set->term_current.TermTransparent == SW_ONOFF_ON) ? "pngalpha" : "png16m",
                          (int) floor(c->set->term_current.dpi),
                          (c->set->term_current.TermAntiAlias == SW_ONOFF_ON)
                                  ? "-dGraphicsAlphaBits=4 -dTextAlphaBits=4"
                                  : "-dGraphicsAlphaBits=0 -dTextAlphaBits=0",
                          GSOutputTemp, comm.EPSFilename);
        BITMAP_TERMINAL_CLEANUP("ghostscript", "png", 1)
    } else if (termtype == SW_TERMTYPE_JPG) // JPEG terminal
    {
        ppl_snprintfTrunc(msg, sizeof(msg), "%s %s -sDEVICE=jpeg -dJPEGQ=100 -r%d %s -sOutputFile=%s %s",
                          GHOSTSCRIPT_COMMAND, GHOSTSCRIPT_STANDARD_FLAGS, (int) floor(c->set->term_current.dpi),
                          (c->set->term_current.TermAntiAlias == SW_ONOFF_ON)
                                  ? "-dGraphicsAlphaBits=4 -dTextAlphaBits=4"
                                  : "-dGraphicsAlphaBits=0 -dTextAlphaBits=0",
                          GSOutputTemp, comm.EPSFilename);
        BITMAP_TERMINAL_CLEANUP("ghostscript", "jpeg", 1)
    } else if (termtype == SW_TERMTYPE_GIF) // GIF terminal
    {
        ppl_snprintfTrunc(
                msg, sizeof(msg), "%s -density %d %s %s %s %s", CONVERT_COMMAND, (int) floor(c->set->term_current.dpi),
                (c->set->term_current.TermTransparent == SW_ONOFF_ON) ? "-alpha activate" : "-alpha deactivate",
                (c->set->term_current.TermAntiAlias == SW_ONOFF_ON) ? "-antialias" : "+antialias", comm.EPSFilename,
                GSOutputTemp);
        BITMAP_TERMINAL_CLEANUP("ImageMagick", "gif", 1);
    } else if (termtype == SW_TERMTYPE_BMP) // BMP terminal
    {
        ppl_snprintfTrunc(msg, sizeof(msg), "%s %s -sDEVICE=bmp16m -r%d %s -sOutputFile=%s %s", GHOSTSCRIPT_COMMAND,
                          GHOSTSCRIPT_STANDARD_FLAGS, (int) floor(c->set->term_current.dpi),
                          (c->set->term_current.TermAntiAlias == SW_ONOFF_ON)
                                  ? "-dGraphicsAlphaBits=4 -dTextAlphaBits=4"
                                  : "-dGraphicsAlphaBits=0 -dTextAlphaBits=0",
                          GSOutputTemp, comm.EPSFilename);
        BITMAP_TERMINAL_CLEANUP("ghostscript", "bmp", 1)
    } else if (termtype == SW_TERMTYPE_TIF) // TIF terminal
    {
        ppl_snprintfTrunc(msg, sizeof(msg), "%s %s -sDEVICE=tiff24nc -r%d %s -sOutputFile=%s %s", GHOSTSCRIPT_COMMAND,
                          GHOSTSCRIPT_STANDARD_FLAGS, (int) floor(c->set->term_current.dpi),
                          (c->set->term_current.TermAntiAlias == SW_ONOFF_ON)
                                  ? "-dGraphicsAlphaBits=4 -dTextAlphaBits=4"
                                  : "-dGraphicsAlphaBits=0 -dTextAlphaBits=0",
                          GSOutputTemp, comm.EPSFilename);
        BITMAP_TERMINAL_CLEANUP("ghostscript", "tif", 1)
    } else if (termtype == SW_TERMTYPE_SVG) // SVG terminal
    {
        ppl_snprintfTrunc(msg, sizeof(msg), "%s %s -sDEVICE=svg -sOutputFile=%s %s > /dev/null 2> /dev/null",
                          GHOSTSCRIPT_COMMAND, GHOSTSCRIPT_STANDARD_FLAGS, GSOutputTemp, comm.EPSFilename);
        BITMAP_TERMINAL_CLEANUP("ghostscript", "svg", 0)
    }

    // Return to user's current working directory
    if (chdir(c->errcontext.session_default.cwd) < 0) {
        ppl_fatal(&c->errcontext, __FILE__, __LINE__, "chdir into cwd failed.");
    }
    lock = 0;
    return;
}

// Run LaTeX to turn set of strings for typesetting into eps code
#define FPRINTF_LINECOUNT(X) \
    fprintf(output, "%s", X); \
    for (i = 0; X[i] != '\0'; i++) \
        if (X[i] == '\n') linecount++;

//! canvas_CallLaTeX - Write all of the text items on the canvas into a LaTeX document, typeset it with LaTeX, and read
//! the resulting DVI file. LaTeX errors are traced back to the text item which caused them.
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void canvas_CallLaTeX(EPSComm *x) {
    char msg[LSTR_LENGTH];
    int linecount = 1, i, j, pid, LatexStatus, LatexStdIn, LatexOut;
    char filename[FNAME_LENGTH], *str_buffer;
    FILE *output;
    listIterator *ListIter;
    CanvasTextItem *TempTextItem, *SuspectTextItem = NULL;
    struct timespec waitperiod; // A time.h timespec specifier for a wait of zero seconds
    unsigned char FirstIter;
    fd_set readable;
    sigset_t sigs;

    int ErrLineNo = 0, ErrReadState = 0, ErrReadPos = 0, NCharsRead = 0, ReadErrorState = 0, TrialNumber;
    char ErrFilename[FNAME_LENGTH] = "", ErrMsg[FNAME_LENGTH] = "";
    char TempErrFilename[FNAME_LENGTH], TempErrLineNo[FNAME_LENGTH], TempErrMsg[FNAME_LENGTH];

    if (ppl_listLen(x->TextItems) < 1) return; // We have no text to give to latex, and so don't need to fork

    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);

    const char TextHeader1[] = "\\documentclass{article}\n\\pagestyle{empty}\n\\usepackage{upgreek}\n";
    const char TextHeader2[] =
            "\\begin{document}\n\\begin{center}{\\bf This page has been left deliberately blank.}\\end{center}\n\\newpage\n";
    const char ItemHeader[] = "\\vbox{\\hbox{\n";
    const char ItemFooter[] =
            "\n\\hskip 0pt plus 1filll minus 1filll}\n\\vskip 0pt plus 1filll minus 1filll}\n\\newpage\n";
    const char TextFooter[] = "\\end{document}\n";

    // chdir into temporary directory so that LaTeX's mess goes into /tmp
    if (chdir(x->c->errcontext.session_default.tempdir) < 0) {
        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Could not chdir into temporary directory.");
        *(x->status) = 1;
        return;
    }

    // Make string buffer
    str_buffer = (char *) malloc(LSTR_LENGTH);
    if (str_buffer == NULL) {
        ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (y).");
        return;
    }

    // Start writing LaTeX document
    ppl_snprintfTrunc(filename, sizeof(filename), "%s.tex", x->TeXFilename);
    output = fopen(filename, "w");
    if (output == NULL) {
        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Could not create temporary LaTeX document");
        *(x->status) = 1;
        free(str_buffer);
        str_buffer = NULL;
        return;
    }
    FPRINTF_LINECOUNT(TextHeader1);
    FPRINTF_LINECOUNT(x->c->set->term_current.LatexPreamble);
    FPRINTF_LINECOUNT(TextHeader2);

    // Sequentially print out text strings
    ListIter = ppl_listIterateInit(x->TextItems);
    while (ListIter != NULL) {
        TempTextItem = (CanvasTextItem *) ListIter->data;
        TempTextItem->LaTeXstartline = linecount;
        FPRINTF_LINECOUNT(ItemHeader);
        FPRINTF_LINECOUNT(TempTextItem->text);
        FPRINTF_LINECOUNT(ItemFooter);
        TempTextItem->LaTeXendline = linecount;
        ppl_listIterate(&ListIter);
    }

    // Finish writing LaTeX document
    FPRINTF_LINECOUNT(TextFooter);
    fclose(output);

    // Fork LaTeX process
    pplcsp_forkLaTeX(x->c, filename, &pid, &LatexStdIn, &LatexOut);

    // Wait for latex process's stdout to become readable. Get bored if this takes too long.
    FirstIter = 1;
    LatexStatus = 0;
    while (1) {
        for (TrialNumber = 1;;) {
            waitperiod.tv_sec = FirstIter ? 15 : 4; // Wait 15 seconds first time around; otherwise wait 4 seconds
            waitperiod.tv_nsec = 0;
            if (ReadErrorState) {
                waitperiod.tv_nsec = 500000000;
                waitperiod.tv_sec = 0;
            } // If we've had an error message, only wait 0.5 sec
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg), "pselect waiting for %ld seconds and %ld nanoseconds",
                                  waitperiod.tv_sec, waitperiod.tv_nsec);
                ppl_log(&x->c->errcontext, msg);
            }
            FD_ZERO(&readable);
            FD_SET(LatexOut, &readable);
            if (pselect(LatexOut + 1, &readable, NULL, NULL, &waitperiod, NULL) == -1) {
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "pselect returned -1. errno=%d", errno);
                    ppl_log(&x->c->errcontext, msg);
                }
                if ((errno == EINTR) && (TrialNumber < 3)) {
                    TrialNumber++;
                    continue;
                }
                LatexStatus = 1;
                break;
            }
            if (DEBUG) {
                ppl_snprintfTrunc(msg, sizeof(msg), "pipe is %s",
                                  (FD_ISSET(LatexOut, &readable)) ? "readable" : "unreadable");
                ppl_log(&x->c->errcontext, msg);
            }
            break;
        }
        if (LatexStatus) break;
        FirstIter = 0;
        if (!FD_ISSET(LatexOut, &readable)) {
            LatexStatus = 1;
            if (DEBUG) ppl_log(&x->c->errcontext, "latex's output pipe has not become readable within time limit");
            break;
        } else {
            if ((NCharsRead = read(LatexOut, str_buffer, LSTR_LENGTH)) > 0) {
                for (j = 0; j < NCharsRead; j++) {
                    if (str_buffer[j] == '\n') {
                        if (ErrReadState == 2) {
                            TempErrMsg[ErrReadPos] = '\0';
                            ppl_snprintfTrunc(ErrFilename, sizeof(ErrFilename), "%s", TempErrFilename);
                            ppl_snprintfTrunc(ErrMsg, sizeof(ErrMsg), "%s", TempErrMsg);
                            ErrLineNo = (int) ppl_getFloat(TempErrLineNo, NULL);
                            if (DEBUG) {
                                ppl_snprintfTrunc(msg, sizeof(msg), "Received LaTeX error on line %d of file <%s>",
                                                  ErrLineNo, ErrFilename);
                                ppl_log(&x->c->errcontext, msg);
                                ppl_snprintfTrunc(msg, sizeof(msg), "Error message: <%s>", ErrMsg);
                                ppl_log(&x->c->errcontext, msg);
                            }
                        }
                        ErrReadState = ErrReadPos = 0;
                    } else if (str_buffer[j] == ':') {
                        if (ErrReadState == 0) {
                            TempErrFilename[ErrReadPos] = '\0';
                            ErrReadPos = 0;
                            if (strcmp(TempErrFilename, "! LaTeX Error") == 0) {
                                ppl_snprintfTrunc(TempErrFilename, sizeof(TempErrFilename), "%s", filename);
                                strcpy(TempErrLineNo, "-1");
                                ErrReadState = 2;
                                ReadErrorState = 1;
                                if (DEBUG) ppl_log(&x->c->errcontext, "Encountered line beginning \"! LaTeX Error\"");
                            } else {
                                ErrReadState = 1;
                            }
                        } else if (ErrReadState == 1) {
                            TempErrLineNo[ErrReadPos] = '\0';
                            ErrReadPos = 0;
                            ErrReadState++;
                            if (!ppl_validFloat(TempErrLineNo, NULL)) ErrReadState++;
                            else ReadErrorState = 1;
                        } else if (ErrReadState == 2) {
                            TempErrMsg[ErrReadPos++] = str_buffer[j];
                        }
                    } else {
                        if (ErrReadState == 0) {
                            TempErrFilename[ErrReadPos++] = str_buffer[j];
                        } else if (ErrReadState == 1) {
                            TempErrLineNo[ErrReadPos++] = str_buffer[j];
                        } else if (ErrReadState == 2) {
                            TempErrMsg[ErrReadPos++] = str_buffer[j];
                        }
                    }
                    if (ErrReadPos > FNAME_LENGTH - 1) ErrReadPos = FNAME_LENGTH - 1;
                }
            } else {
                break;
            } // read function returned zero; indicates EOF
        }
    }

    // Finish talking to LaTeX process
    close(LatexStdIn); // This has the effect of killing latex
    close(LatexOut);
    free(str_buffer);
    str_buffer = NULL;
    sigprocmask(SIG_UNBLOCK, &sigs, NULL);

    // Return error message if latex has failed
    if (LatexStatus) {
        ppl_strStrip(ErrMsg, ErrMsg);
        if (strcmp(filename, ErrFilename) == 0) // Case 1: Error in the LaTeX file which we just generated
        {
            unsigned char ExactHit =
                    0; // Cycle through all text items and see whether error line number is in the middle of one of them
            ListIter = ppl_listIterateInit(x->TextItems);
            while (ListIter != NULL) {
                TempTextItem = (CanvasTextItem *) ListIter->data;
                if (TempTextItem->LaTeXstartline > ErrLineNo)
                    break; // We have gone past the line number where the error happened
                SuspectTextItem = TempTextItem;
                ExactHit = (TempTextItem->LaTeXendline >=
                            ErrLineNo); // If this is zero, then error was after the end of the item we just looked at
                ppl_listIterate(&ListIter);
            }
            if (SuspectTextItem == NULL)
                ppl_snprintfTrunc(msg, sizeof(msg), "LaTeX error encountered in an unidentifiable canvas item.");
            else if (ExactHit)
                ppl_snprintfTrunc(msg, sizeof(msg), "LaTeX error encountered in text string in canvas item %d.",
                                  SuspectTextItem->CanvasMultiplotID);
            else
                ppl_snprintfTrunc(msg, sizeof(msg),
                                  "LaTeX error encountered at the end of text string in canvas item %d.",
                                  SuspectTextItem->CanvasMultiplotID);
            ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
            ppl_snprintfTrunc(msg, sizeof(msg), "Error was: %s",
                              ErrMsg); // Output the actual error which LaTeX returned to us
            ppl_error(&x->c->errcontext, ERR_PREFORMED, -1, -1, msg);
            if (SuspectTextItem != NULL) {
                canvas_item *ptr = ((canvas_itemlist *) x->c->canvas_items)
                                           ->first; // Cycle through all canvas items to find the culprit
                for (ptr = ((canvas_itemlist *) x->c->canvas_items)->first;
                     ((ptr != NULL) && (ptr->id != SuspectTextItem->CanvasMultiplotID)); ptr = ptr->next);
                if (ptr != NULL) {
                    ppl_snprintfTrunc(
                            msg, sizeof(msg),
                            "\nInput Pyxplot command was:\n\n"); // Now tell the user what Pyxplot command produced the culprit
                    i = strlen(msg);
                    ppl_canvas_item_textify(x->c, ptr, msg + i, sizeof(msg) - i);
                    i += strlen(msg + i);
                    ppl_strAppendf(msg, sizeof(msg), &i, "\n");
                    ppl_error(&x->c->errcontext, ERR_PREFORMED, -1, -1, msg);
                } // Then output the LaTeX string which produced the error
                ppl_snprintfTrunc(msg, sizeof(msg), "\nOffending input to LaTeX was:\n\n%s\n", SuspectTextItem->text);
                ppl_error(&x->c->errcontext, ERR_PREFORMED, -1, -1, msg);
            }
        } else if ((ErrFilename[0] == '\0') &&
                   (ErrMsg[0] == '\0')) // Case 2: Error encountered, but we didn't catch error message
        {
            ppl_snprintfTrunc(msg, sizeof(msg), "Unidentified LaTeX error encountered.");
            ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
        } else // Case 3: Error in another file which the user seems to have imported
        {
            ppl_snprintfTrunc(msg, sizeof(msg), "LaTeX error encountered in imported file <%s> on line %d.",
                              ErrFilename, ErrLineNo);
            ppl_error(&x->c->errcontext, ERR_GENERIC, -1, -1, msg);
            ppl_snprintfTrunc(msg, sizeof(msg), "Error was: %s", ErrMsg);
            ppl_error(&x->c->errcontext, ERR_PREFORMED, -1, -1, msg);
        }
        if (chdir(x->c->errcontext.session_default.cwd) < 0) {
            ppl_fatal(&x->c->errcontext, __FILE__, __LINE__, "chdir into cwd failed.");
        }
        *(x->status) = 1;
        return;
    }

    // Convert dvi into postscript fragments
    ppl_snprintfTrunc(filename, sizeof(filename), "%s.dvi", x->TeXFilename);
    x->dvi = ReadDviFile(&x->c->errcontext, filename, x->status);
    if (*(x->status)) return; // DVI interpreter failed

    // Return to user's current working directory after LaTeX has finished making a mess
    if (chdir(x->c->errcontext.session_default.cwd) < 0) {
        ppl_fatal(&x->c->errcontext, __FILE__, __LINE__, "chdir into cwd failed.");
    }
    return;
}

//! canvas_MakeEPSBuffer - Create a temporary buffer file into which the postscript fragments for each canvas item are
//! written, before the overall bounding box is known
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void canvas_MakeEPSBuffer(EPSComm *x) {
    x->epsbuffer = tmpfile();
    if (x->epsbuffer == NULL) {
        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Could not create temporary eps buffer file.");
        *(x->status) = 1;
        return;
    }
    return;
}

//! fontListedEarlier - Test whether a font in a list of the fonts used in a DVI file is derived from the same type 1
//! font as an earlier font in the list, in which case the type 1 font only needs to be included once
//! \param [in] fonts - The list of fonts (dviFontDetails structures)
//! \param [in] font - A font in the list
//! \return True if an earlier font in the list has the same <basePsName>

static int fontListedEarlier(list *fonts, const dviFontDetails *font) {
    listIterator *iter = ppl_listIterateInit(fonts);
    while ((iter != NULL) && (iter->data != font)) {
        if (strcmp(((dviFontDetails *) iter->data)->basePsName, font->basePsName) == 0) return 1;
        ppl_listIterate(&iter);
    }
    return 0;
}

//! canvas_EPSWrite - Write the final postscript file, with a header giving the overall bounding box, followed by the
//! postscript fragments from the temporary buffer file
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void canvas_EPSWrite(EPSComm *x) {
    char msg[PPL_MSG_LENGTH];
    int i;
    double PAGEwidth, PAGEheight, margin_left, margin_top, margin_bottom;
    FILE *epsout, *PFAfile;
    char LandscapifyText[FNAME_LENGTH], EnlargementText[FNAME_LENGTH], *PaperName, *PFAfilename;
    listIterator *ListIter;

    // Check that we have a bounding box
    if (!x->bb_set) { x->bb_left = x->bb_right = x->bb_bottom = x->bb_top = 0.0; }

    // Apply enlarge and landscape terminals as required
    LandscapifyText[0] = EnlargementText[0] = '\0';
    if (x->c->set->term_current.landscape == SW_ONOFF_ON) canvas_EPSLandscapify(x, LandscapifyText);
    if ((x->c->set->term_current.TermEnlarge == SW_ONOFF_ON) && (x->termtype == SW_TERMTYPE_PS))
        canvas_EPSEnlarge(x, EnlargementText);


    // Work out name to call papersize
    PaperName = x->c->set->term_current.PaperName;
    if (strcmp(PaperName, "User-defined papersize") == 0) PaperName = "CustomPageSize";
    // Return to user's current working directory
    if (chdir(x->c->errcontext.session_default.cwd) < 0) {
        ppl_fatal(&x->c->errcontext, __FILE__, __LINE__, "chdir into cwd failed.");
    }

    // Work out some sensible margins for page
    PAGEwidth = x->c->set->term_current.PaperWidth.real;
    PAGEheight = x->c->set->term_current.PaperHeight.real;
    margin_left = PAGEwidth / 14;
    margin_top = PAGEheight / 14;
    margin_left *= M_TO_PS;
    margin_top *= M_TO_PS;
    margin_bottom = margin_top;

    // Open output postscript file for writing
    if ((epsout = fopen(x->EPSFilename, "w")) == NULL) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Could not open file '%s' for writing.", x->EPSFilename);
        ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
        *(x->status) = 1;
        return;
    }

    // Write EPS header
    if (x->c->set->term_current.TermType != SW_TERMTYPE_PS) {
        fprintf(epsout, "%s%s\n", "%!PS-Adobe-3.0", " EPSF-3.0");
        fprintf(epsout, "%%%%BoundingBox: %d %d %d %d\n", (int) floor(x->bb_left), (int) floor(x->bb_bottom),
                (int) ceil(x->bb_right), (int) ceil(x->bb_top));
        fprintf(epsout, "%%%%HiResBoundingBox: %f %f %f %f\n", x->bb_left, x->bb_bottom, x->bb_right, x->bb_top);
    } else {
        fprintf(epsout, "%s%s\n", "%!PS-Adobe-3.0", "");
        fprintf(epsout, "%%%%BoundingBox: %d %d %d %d\n", (int) floor(margin_left), (int) floor(margin_bottom),
                (int) ceil(x->bb_right - x->bb_left + margin_left),
                (int) ceil(x->bb_top - x->bb_bottom + margin_bottom));
        fprintf(epsout, "%%%%HiResBoundingBox: %f %f %f %f\n", margin_left, margin_bottom,
                x->bb_right - x->bb_left + margin_left, x->bb_top - x->bb_bottom + margin_bottom);
    }
    fprintf(epsout, "%%%%Creator: (Pyxplot %s -- executing %s:%d)\n", VERSION, x->c->errcontext.error_input_filename,
            x->c->errcontext.error_input_linenumber);
    fprintf(epsout, "%%%%Title: (%s)\n", x->title);
    fprintf(epsout, "%%%%CreationDate: (%s)\n", ppl_strStrip(ppl_friendlyTimestring(), msg));
    if (x->c->set->term_current.TermType == SW_TERMTYPE_PS) fprintf(epsout, "%%%%Pages: 1\n");
    fprintf(epsout, "%%%%DocumentData: Clean7Bit\n");
    if (x->c->set->term_current.landscape == SW_ONOFF_ON) fprintf(epsout, "%%%%Orientation: Landscape\n");
    else fprintf(epsout, "%%%%Orientation: Portrait\n");
    if (x->c->set->term_current.TermType == SW_TERMTYPE_PS)
        fprintf(epsout, "%%%%DocumentMedia: %s %d %d white { }\n", PaperName,
                (int) (x->c->set->term_current.PaperWidth.real * M_TO_PS),
                (int) (x->c->set->term_current.PaperHeight.real * M_TO_PS));
    fprintf(epsout, "%%%%DocumentFonts:"); // %%DocumentFonts has a list of all of the fonts that we use
    if (x->dvi != NULL) ListIter = ppl_listIterateInit(x->dvi->fonts);
    else ListIter = NULL;
    while (ListIter != NULL) {
        const dviFontDetails *const font = (dviFontDetails *) ListIter->data;
        if (!fontListedEarlier(x->dvi->fonts, font)) fprintf(epsout, " %s", font->basePsName);
        ppl_listIterate(&ListIter);
    }
    fprintf(epsout, "\n");
    fprintf(epsout, "%%%%EndComments\n\n");

    // In postscript files, now set up page size
    if (x->c->set->term_current.TermType == SW_TERMTYPE_PS) {
        fprintf(epsout, "%%%%BeginDefaults\n");
        fprintf(epsout, "%%%%PageMedia: %s\n", PaperName);
        fprintf(epsout, "%%%%EndDefaults\n");
    }

    // Write EPS prolog
    fprintf(epsout, "%%%%BeginProlog\n");

    // Output all of the fonts which we're going to use
    if (chdir(x->c->errcontext.session_default.tempdir) < 0) {
        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Could not chdir into temporary directory.");
        *(x->status) = 1;
        fclose(epsout);
        return;
    }
    if (x->dvi != NULL) ListIter = ppl_listIterateInit(x->dvi->fonts);
    else ListIter = NULL;
    while (ListIter != NULL) {
        const dviFontDetails *const font = (dviFontDetails *) ListIter->data;
        PFAfilename = font->pfaPath;
        // PFAfilename==NULL indicates that this is a built-in font that doesn't require a fontdef. Several TeX fonts
        // may be derived from the same type 1 font (e.g. ecrm1000 and tcrm1000), which only needs including once.
        if ((PFAfilename != NULL) && !fontListedEarlier(x->dvi->fonts, font)) {
            fprintf(epsout, "%%%%BeginFont: %s\n", font->basePsName);
            PFAfile = fopen(PFAfilename, "r");
            if (PFAfile == NULL) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Could not open pfa file '%s'", PFAfilename);
                ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
                *(x->status) = 1;
                fclose(epsout);
                return;
            }
            while (fgets(msg, FNAME_LENGTH, PFAfile) != NULL)
                if (fputs(msg, epsout) == EOF) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Error while writing to file '%s'.", x->EPSFilename);
                    ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
                    *(x->status) = 1;
                    fclose(PFAfile);
                    fclose(epsout);
                    return;
                }
            fclose(PFAfile);
            fprintf(epsout, "\n%%%%EndFont\n");
        }
        ppl_listIterate(&ListIter);
    }
    if (chdir(x->c->errcontext.session_default.cwd) < 0) {
        ppl_fatal(&x->c->errcontext, __FILE__, __LINE__, "chdir into cwd failed.");
    }

    // Define re-encoded, slanted or extended versions of fonts, as instructed by the TeX font map
    if (x->dvi != NULL) ListIter = ppl_listIterateInit(x->dvi->fonts);
    else ListIter = NULL;
    while (ListIter != NULL) {
        const dviFontDetails *const font = (dviFontDetails *) ListIter->data;
        if (font->fontSetup != NULL) fprintf(epsout, "%s", font->fontSetup);
        ppl_listIterate(&ListIter);
    }

    // Output macros which Pyxplot needs
    if (x->c->set->term_current.TermType == SW_TERMTYPE_PS) fprintf(epsout, "%s", PS_PROLOG_TEXT);
    fprintf(epsout, "%s", EPS_PROLOG_TEXT);
    fprintf(epsout, "/ps { 1 } def\n/ps75 { ps .75 mul } def\n"); // Pointsize variables

    if (x->PointTypesUsed[28]) { x->PointTypesUsed[14] = x->PointTypesUsed[0] = 1; } // pt6 depends upon pt4 and pt5
    if (x->PointTypesUsed[29]) { x->PointTypesUsed[15] = x->PointTypesUsed[1] = 1; } // pt9 depends upon pt7 and pt8
    if (x->PointTypesUsed[30]) { x->PointTypesUsed[16] = x->PointTypesUsed[2] = 1; } // pt12 depends upon pt10 and pt11
    if (x->PointTypesUsed[31]) { x->PointTypesUsed[17] = x->PointTypesUsed[3] = 1; } // pt15 depends upon pt13 and pt14
    if (x->PointTypesUsed[32]) { x->PointTypesUsed[18] = x->PointTypesUsed[4] = 1; } // pt18 depends upon pt16 and pt17
    if (x->PointTypesUsed[33]) { x->PointTypesUsed[19] = x->PointTypesUsed[5] = 1; } // pt21 depends upon pt19 and pt20
    if (x->PointTypesUsed[34]) { x->PointTypesUsed[20] = x->PointTypesUsed[6] = 1; } // pt24 depends upon pt22 and pt23
    if (x->PointTypesUsed[35]) { x->PointTypesUsed[21] = x->PointTypesUsed[7] = 1; } // pt27 depends upon pt25 and pt26
    if (x->PointTypesUsed[36]) { x->PointTypesUsed[22] = x->PointTypesUsed[8] = 1; } // pt30 depends upon pt28 and pt29
    if (x->PointTypesUsed[37]) { x->PointTypesUsed[23] = x->PointTypesUsed[9] = 1; } // pt33 depends upon pt31 and pt32
    if (x->PointTypesUsed[38]) { x->PointTypesUsed[24] = x->PointTypesUsed[10] = 1; } // pt36 depends upon pt34 and pt35
    if (x->PointTypesUsed[39]) { x->PointTypesUsed[25] = x->PointTypesUsed[11] = 1; } // pt33 depends upon pt37 and pt38
    if (x->PointTypesUsed[40]) { x->PointTypesUsed[26] = x->PointTypesUsed[12] = 1; } // pt3 depends upon pt1 and pt2
    if (x->PointTypesUsed[41]) { x->PointTypesUsed[27] = x->PointTypesUsed[13] = 1; } // pt6 depends upon pt4 and pt5

    if (x->StarTypesUsed[6]) { x->StarTypesUsed[5] = 1; } // st7 depends upon st6
    for (i = 0; i < N_POINTTYPES; i++)
        if (x->PointTypesUsed[i]) fprintf(epsout, "%s\n", eps_PointTypes[i]);
    for (i = 0; i < N_STARTYPES; i++)
        if (x->StarTypesUsed[i]) {
            fprintf(epsout, "%s\n", eps_StarCore);
            break;
        }
    for (i = 0; i < N_STARTYPES; i++)
        if (x->StarTypesUsed[i]) fprintf(epsout, "%s\n", eps_StarTypes[i]);
    fprintf(epsout, "%%%%EndProlog\n\n");

    // In postscript files, now set up page 1
    if (x->c->set->term_current.TermType == SW_TERMTYPE_PS) {
        fprintf(epsout, "<< /PageSize [ %d %d ] >> setpagedevice\n",
                (int) (x->c->set->term_current.PaperWidth.real * M_TO_PS),
                (int) (x->c->set->term_current.PaperHeight.real * M_TO_PS));
        fprintf(epsout, "%%%%Page: 1 1\n");
        fprintf(epsout, "%%%%BeginPageSetup\n");
        fprintf(epsout, "/pgsave save def\n");
        fprintf(epsout, "%%%%EndPageSetup\n\n");
    }

    // Now write any global transformations needed by the enlarge and landscape terminals.
    if (x->c->set->term_current.TermType == SW_TERMTYPE_PS)
        fprintf(epsout, "%.2f %.2f translate\n", margin_left - x->bb_left, margin_bottom - x->bb_bottom);
    if ((LandscapifyText[0] != '\0') || (EnlargementText[0] != '\0')) {
        fprintf(epsout, "%% Global transformations to give %soutput%s\n",
                (LandscapifyText[0] != '\0') ? "landscape " : "",
                (EnlargementText[0] != '\0') ? " which fills the page" : "");
        fprintf(epsout, "%s", EnlargementText);
        fprintf(epsout, "%s", LandscapifyText);
        fprintf(epsout, "\n");
    }

    // Copy contents of eps buffer into postscript output
    fflush(x->epsbuffer);
    rewind(x->epsbuffer);
    while (fgets(msg, FNAME_LENGTH, x->epsbuffer) != NULL)
        if (fputs(msg, epsout) == EOF) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Error while writing to file '%s'.", x->EPSFilename);
            ppl_error(&x->c->errcontext, ERR_FILE, -1, -1, msg);
            *(x->status) = 1;
            fclose(epsout);
            return;
        }

    // Write postscript footer and finish
    if (x->c->set->term_current.TermType == SW_TERMTYPE_PS) fprintf(epsout, "pgsave restore\n"); // End of page
    fprintf(epsout, "showpage\n%%%%EOF\n"); // End of document
    fclose(x->epsbuffer);
    x->epsbuffer = NULL;
    fclose(epsout);
    return;
}

//! canvas_EPSRenderTextItem - Write the postscript for a text item, taken from the typeset DVI output
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [out] strout - Set to point to a string containing the postscript, or NULL to write it directly to the output
//! file
//! \param [in] pageno - The page number of the text item within the DVI output
//! \param [in] xpos - The x position of the text item's anchor point, in postscript points
//! \param [in] ypos - The y position of the text item's anchor point, in postscript points
//! \param [in] halign - The horizontal alignment of the text relative to its anchor point, one of the SW_HALIGN_*
//! constants
//! \param [in] valign - The vertical alignment of the text relative to its anchor point, one of the SW_VALIGN_*
//! constants
//! \param [in] colstr - The postscript command to set the color of the text
//! \param [in] fontsize - The font size, as a multiple of the default font size
//! \param [in] rotate - The angle by which to rotate the text, in radians
//! \param [out] width - Set to the width of the text item, in postscript points (optional)
//! \param [out] height - Set to the height of the text item, in postscript points (optional)

void canvas_EPSRenderTextItem(EPSComm *x, char **strout, int pageno, double xpos, double ypos, const int halign,
                              const int valign, char *colstr, double fontsize, double rotate, double *width,
                              double *height) {
    postscriptPage *dviPage;
    listIterator *ListIter;
    char *cptr, *out;
    static char *buffer = NULL;
    const long BUFLEN = 131072;
    long bufpos = 0;
    double bb_left, bb_right, bb_top, bb_bottom, xanchor, yanchor;
    double ab_left, ab_right, ab_top,
            ab_bottom; // The align box is different from the bounding box in that it gives the baseline height of the lowest line of text and the cap height of the highest line of text

    // If colstr is blank, we are painting with null ink
    if (colstr[0] == '\0') return;

    // Write output to text buffer initially
    if (buffer == NULL) buffer = malloc(BUFLEN); // memory leak here!
    if (buffer == NULL) return;
    out = buffer;

    if (x->dvi == NULL) {
        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1,
                  "Attempting to display a text item before latex has generated it");
        return;
    }

    // Fetch requested page of postscript
    dviPage = (postscriptPage *) ppl_listGetItem(x->dvi->output->pages, pageno + 1);
    if (dviPage == NULL) {
        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Not all text items were rendered by LaTeX");
        *(x->status) = 1;
        return;
    }
    bb_left = dviPage->boundingBox[0];
    bb_bottom = dviPage->boundingBox[1];
    bb_right = dviPage->boundingBox[2];
    bb_top = dviPage->boundingBox[3];
    ab_left = dviPage->textSizeBox[0];
    ab_bottom = dviPage->textSizeBox[1];
    ab_right = dviPage->textSizeBox[2];
    ab_top = dviPage->textSizeBox[3];

    // Return dimensions of text item
    if (width != NULL) *width = (bb_right - bb_left) * fontsize;
    if (height != NULL) *height = (bb_top - bb_bottom) * fontsize;

    // Work out where our anchor point is on postscript
    if (halign == SW_HALIGN_LEFT) xanchor = ab_left;
    else if (halign == SW_HALIGN_CENT) xanchor = (ab_left + ab_right) / 2.0;
    else if (halign == SW_HALIGN_RIGHT) xanchor = ab_right;
    else {
        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Illegal halign value passed to canvas_EPSRenderTextItem");
        *(x->status) = 1;
        return;
    }
    if (valign == SW_VALIGN_TOP) yanchor = bb_top;
    else if (valign == SW_VALIGN_CENT) yanchor = (ab_top + ab_bottom) / 2.0;
    else if (valign == SW_VALIGN_BOT) yanchor = ab_bottom;
    else {
        ppl_error(&x->c->errcontext, ERR_INTERNAL, -1, -1, "Illegal valign value passed to canvas_EPSRenderTextItem");
        *(x->status) = 1;
        return;
    }

    // Update bounding box of canvas. For this, use BOUNDING box, not ALIGNMENT box.
    eps_core_PlotBoundingBox(x,
                             xpos * M_TO_PS + (bb_left - xanchor) * fontsize * cos(rotate) +
                                     (bb_bottom - yanchor) * fontsize * -sin(rotate),
                             ypos * M_TO_PS + (bb_left - xanchor) * fontsize * sin(rotate) +
                                     (bb_bottom - yanchor) * fontsize * cos(rotate),
                             0, 1);
    eps_core_PlotBoundingBox(x,
                             xpos * M_TO_PS + (bb_right - xanchor) * fontsize * cos(rotate) +
                                     (bb_bottom - yanchor) * fontsize * -sin(rotate),
                             ypos * M_TO_PS + (bb_right - xanchor) * fontsize * sin(rotate) +
                                     (bb_bottom - yanchor) * fontsize * cos(rotate),
                             0, 1);
    eps_core_PlotBoundingBox(x,
                             xpos * M_TO_PS + (bb_left - xanchor) * fontsize * cos(rotate) +
                                     (bb_top - yanchor) * fontsize * -sin(rotate),
                             ypos * M_TO_PS + (bb_left - xanchor) * fontsize * sin(rotate) +
                                     (bb_top - yanchor) * fontsize * cos(rotate),
                             0, 1);
    eps_core_PlotBoundingBox(x,
                             xpos * M_TO_PS + (bb_right - xanchor) * fontsize * cos(rotate) +
                                     (bb_top - yanchor) * fontsize * -sin(rotate),
                             ypos * M_TO_PS + (bb_right - xanchor) * fontsize * sin(rotate) +
                                     (bb_top - yanchor) * fontsize * cos(rotate),
                             0, 1);


    // Perform translation such that postscript text appears in the right place on the page
    snprintf(out + bufpos, BUFLEN - bufpos, "gsave\n");
    bufpos += strlen(out + bufpos);
    snprintf(out + bufpos, BUFLEN - bufpos, "%.2f %.2f translate\n", xpos * M_TO_PS, ypos * M_TO_PS);
    bufpos += strlen(out + bufpos);
    snprintf(out + bufpos, BUFLEN - bufpos, "%.2f rotate\n", rotate * 180 / M_PI);
    bufpos += strlen(out + bufpos);
    snprintf(out + bufpos, BUFLEN - bufpos, "%f %f scale\n", fontsize, fontsize);
    bufpos += strlen(out + bufpos);
    snprintf(out + bufpos, BUFLEN - bufpos, "%.2f %.2f translate\n", -xanchor, -yanchor);
    bufpos += strlen(out + bufpos);

    // Copy postscript description of page out of dvi buffer
    ListIter = ppl_listIterateInit(dviPage->text);
    while (ListIter != NULL) {
        cptr = (char *) ListIter->data;
        while (*cptr != '\0') {
            if (*cptr != '\x01') snprintf(out + bufpos, BUFLEN - bufpos, "%c", *cptr);
            else
                snprintf(out + bufpos, BUFLEN - bufpos, "%s",
                         colstr); // ASCII x01 is a magic code to tell us to revert to default color
            bufpos += strlen(out + bufpos);
            cptr++;
        }
        ppl_listIterate(&ListIter);
    }

    // Undo translation and we're finished
    snprintf(out + bufpos, BUFLEN - bufpos, "grestore\n");
    bufpos += strlen(out + bufpos);

    // Either return string, or write output to file
    if (strout == NULL) fprintf(x->epsbuffer, "%s", buffer);
    else *strout = buffer;
    return;
}

//! canvas_EPSLandscapify - Convert a portrait EPS file to landscape orientation, by changing its bounding box and
//! adding a coordinate transformation to the top of the file
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [out] transform - Character buffer into which to write the postscript coordinate transformation

void canvas_EPSLandscapify(EPSComm *x, char *transform) {
    double width, height;
    width = x->bb_right - x->bb_left;
    height = x->bb_top - x->bb_bottom;
    sprintf(transform, "90 rotate\n%f %f translate\n", -x->bb_left, -x->bb_top);
    x->bb_left = 0.0;
    x->bb_bottom = 0.0;
    x->bb_right = height;
    x->bb_top = width;
    return;
}

//! canvas_EPSEnlarge - Enlarge an EPS file to fill the page, by changing its bounding box and adding a coordinate
//! transformation to the top of the file
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [out] transform - Character buffer into which to write the postscript coordinate transformation

void canvas_EPSEnlarge(EPSComm *x, char *transform) {
    double EPSwidth, EPSheight, PAGEwidth, PAGEheight;
    double margin_left, margin_right, margin_top, margin_bottom;
    double scaling_x, scaling_y, translate_x, translate_y;

    // Read dimensions of page
    PAGEwidth = x->c->set->term_current.PaperWidth.real;
    PAGEheight = x->c->set->term_current.PaperHeight.real;

    // Calculate dimensions of EPS image
    EPSwidth = x->bb_right - x->bb_left;
    EPSheight = x->bb_top - x->bb_bottom;
    if ((EPSwidth <= 0.0) || (EPSheight <= 0.0)) return; // Don't attempt to enlarge pages of size zero

    // Work out some sensible margins for page
    margin_left = PAGEwidth / 14;
    margin_top = PAGEheight / 14;
    if (margin_left > 15e-3) margin_left = 15e-3; // mm
    if (margin_top > 15e-3) margin_top = 15e-3; // mm
    margin_right = margin_left;
    margin_bottom = margin_top;

    // Work out what scaling factors will make page fit
    scaling_x = (PAGEwidth - margin_left - margin_right) / EPSwidth * M_TO_PS;
    scaling_y = (PAGEheight - margin_top - margin_bottom) / EPSheight * M_TO_PS;
    if ((!gsl_finite(scaling_x)) || (!gsl_finite(scaling_y))) return; // Don't attempt to enlarge pages of size zero
    if (scaling_y < scaling_x) scaling_x = scaling_y; // Lowest scaling factor is biggest enlargement we can do

    // Work out how to translate origin to (0,0)
    translate_x = margin_left * M_TO_PS - x->bb_left * scaling_x;
    translate_y = PAGEheight * M_TO_PS - margin_top * M_TO_PS - x->bb_top * scaling_x;

    // Write out transformation
    sprintf(transform, "%f %f translate\n%f %f scale\n", translate_x, translate_y, scaling_x, scaling_x);
    x->bb_left = margin_left * M_TO_PS;
    x->bb_right = margin_left * M_TO_PS + EPSwidth * scaling_x;
    x->bb_top = (PAGEheight - margin_top) * M_TO_PS;
    x->bb_bottom = (PAGEheight - margin_top) * M_TO_PS - EPSheight * scaling_x;
    return;
}
