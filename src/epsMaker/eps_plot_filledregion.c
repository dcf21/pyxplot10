// eps_plot_filledregion.c
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

// Fills regions of graphs bounded by paths through data points, for the filledregion and yerrorshaded plot styles, and
// for boxes (eps_plot_styles.c). The outline is collected point by point; where it crosses the edges of the graph, it
// is clipped by walking around the edges of the graph between the crossing points, inserting the corners, so that only
// the part of the region within the graph is filled and, optionally, stroked. Points where the outline crosses itself
// flip which side of it is to be filled.

#define _PPL_EPS_PLOT_FILLEDREGION_C 1

#include <stdlib.h>
#include <stdio.h>

#include <gsl/gsl_math.h>

#include "coreUtils/memAlloc.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/list.h"
#include "coreUtils/errorReport.h"

#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_filledregion.h"
#include "epsMaker/eps_plot_linedraw.h"
#include "epsMaker/eps_settings.h"

//! FilledRegion_Init - Begin drawing a filled region (e.g. for the fillcolor of a plot), whose outline is given in data
//! coordinates and which is clipped to the edges of the graph
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] xa - The x axis
//! \param [in] ya - The y axis
//! \param [in] za - The z axis
//! \param [in] xrn - The monotonic region of the x axis in which points lie
//! \param [in] yrn - The monotonic region of the y axis in which points lie
//! \param [in] zrn - The monotonic region of the z axis in which points lie
//! \param [in] sg - The graph settings
//! \param [in] ThreeDim - Flag indicating whether the graph is three-dimensional
//! \param [in] origin_x - The x coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] origin_y - The y coordinate of the bottom-left corner of the graph, in postscript points
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots
//! \return A handle for the filled region, allocated in the current ppl_memAlloc context, or NULL on failure

FilledRegionHandle *FilledRegion_Init(EPSComm *x, pplset_axis *xa, pplset_axis *ya, pplset_axis *za, const int xrn,
                                      const int yrn, const int zrn, pplset_graph *sg, const unsigned char ThreeDim,
                                      const double origin_x, const double origin_y, const double width,
                                      const double height, const double zdepth) {
    FilledRegionHandle *output = (FilledRegionHandle *) ppl_memAlloc(sizeof(FilledRegionHandle));
    if (output == NULL) return NULL;
    output->x = x;
    output->xa = xa;
    output->ya = ya;
    output->za = za;
    output->xrn = xrn;
    output->yrn = yrn;
    output->zrn = zrn;
    output->sg = sg;
    output->ThreeDim = ThreeDim;
    output->origin_x = origin_x;
    output->origin_y = origin_y;
    output->width = width;
    output->height = height;
    output->zdepth = zdepth;
    output->Naxiscrossings = 0;
    output->first = 1;
    output->EverInside = 0;
    output->points = ppl_listInit(0);
    return output;
}

//! FilledRegion_Point - Add a point to the outline of a filled region
//! \param [in] X - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] fr - The handle of the filled region
//! \param [in] x - The x coordinate of the point, in data coordinates
//! \param [in] y - The y coordinate of the point, in data coordinates

void FilledRegion_Point(EPSComm *X, FilledRegionHandle *fr, const double x, const double y) {
    // Variables for placing the point and finding where the outline crosses the edges of the graph
    FilledRegionPoint p;
    double xpos, ypos, depth, xap, yap, zap, ap1, ap2;
    unsigned char f1, f2;
    int NCrossings, i1, i2;
    double cx1, cy1, cz1, cx2, cy2, cz2;

    // Work out where (x,y) in coordinate space lies on the canvas
    eps_plot_GetPosition(&xpos, &ypos, &depth, &xap, &yap, &zap, NULL, NULL, NULL, fr->ThreeDim, x, y, 0, fr->xa,
                         fr->ya, fr->za, fr->xrn, fr->yrn, fr->zrn, fr->sg, fr->origin_x, fr->origin_y, fr->width,
                         fr->height, fr->zdepth, 1);

    // Points which cannot be placed on the page are skipped
    if ((!gsl_finite(xpos)) || (!gsl_finite(ypos)) || (!gsl_finite(depth))) { return; }

    // Add this to a list of points describing the outline of the region which we are going to fill
    p.inside = ((xap >= 0) && (xap <= 1) && (yap >= 0) && (yap <= 1) && (zap >= 0) && (zap <= 1));
    p.FillSideFlip_fwd = 0;
    p.FillSideFlip_prv = 0;
    p.x = xpos;
    p.y = ypos;
    p.xa = x;
    p.ya = y;
    p.xap = xap;
    p.yap = yap;

    // Add the point to the outline, and count how many times the segment from the previous point crosses the edges of
    // the graph
    ppl_listAppendCpy(fr->points, (void *) &p, sizeof(p));

    // Count the number of times which the path crosses the boundary of the clip region
    if (!fr->first) {
        LineDraw_FindCrossingPoints(X, fr->lastx, fr->lasty, 0, fr->lastxap, fr->lastyap, 0.5, xpos, ypos, 0, xap, yap,
                                    0.5, &i1, &i2, &cx1, &cy1, &cz1, &cx2, &cy2, &cz2, &f1, &ap1, &f2, &ap2,
                                    &NCrossings);
        fr->Naxiscrossings += NCrossings;
        fr->EverInside = (fr->EverInside) || (NCrossings > 0) || i1 || i2;
    }
    fr->lastx = xpos;
    fr->lasty = ypos;
    fr->lastxap = xap;
    fr->lastyap = yap;
    fr->first = 0;
    return;
}

//! frac_sorter - Comparison function for qsort(), used to sort the points where a filled region's outline crosses the
//! edge of the graph into order around the edge
//! \param [in] av - Pointer to the first crossing point
//! \param [in] bv - Pointer to the second crossing point
//! \return -1, 0 or 1 depending on the order of the crossing points

static int frac_sorter(const void *av, const void *bv) {
    const FilledRegionAxisCrossing *a = (const FilledRegionAxisCrossing *) av;
    const FilledRegionAxisCrossing *b = (const FilledRegionAxisCrossing *) bv;
    if (b->AxisFace > a->AxisFace) return -1;
    if (b->AxisFace < a->AxisFace) return 1;
    if (b->AxisFace < FACE_BOTTOM) // Sort top from right to left, and left from top to bottom
    {
        if (b->AxisPos > a->AxisPos) return 1;
        else if (b->AxisPos < a->AxisPos) return -1;
    } else // Sort bottom from left to right, and right from bottom to top
    {
        if (b->AxisPos > a->AxisPos) return -1;
        else if (b->AxisPos < a->AxisPos) return 1;
    }
    return 0;
}

//! TestPointInside - Test whether a point lies inside the outline of a filled region, by counting how many times a ray
//! from the point crosses the outline
//! \param [in] fr - The handle of the filled region
//! \param [in] X - The x coordinate of the point, in postscript points
//! \param [in] Y - The y coordinate of the point, in postscript points
//! \param [in] dir_x - The x component of the direction of the ray
//! \param [in] dir_y - The y component of the direction of the ray
//! \return One if the point lies inside the outline, zero otherwise

static unsigned char TestPointInside(const FilledRegionHandle *fr, double X, double Y, const int dir_x,
                                     const int dir_y) {
    int n;

    // Move fractionally off test point
    if (dir_x == -1) X -= fabs(1e-15 * X);
    else if (dir_x == 1) X += fabs(1e-15 * X);
    else if (dir_y == -1) Y -= fabs(1e-15 * Y);
    else if (dir_y == 1) Y += fabs(1e-15 * Y);

    // Count how many segments of the outline cross a ray from the point in the chosen direction; an odd number means
    // the point is inside
    int Npoints = fr->points->length;
    listItem *li2 = fr->points->first;
    FilledRegionPoint *p2 = (FilledRegionPoint *) (li2->data);

    // Loop around perimeter of region which we are going to fill
    for (int i = n = 0; i < Npoints; i++) {
        listItem *li1 = li2;
        FilledRegionPoint *p1 = p2;
        li2 = li1->next;
        if (li2 == NULL) li2 = fr->points->first;
        p2 = (FilledRegionPoint *) (li2->data); // p1 -> p2 is a line segment of perimeter

        // Count how many segments cross lines travelling out up/left/down/right from (X,Y)

        // A vertical ray, or a horizontal one
        if (dir_x == 0) {
            double y_intersect =
                    (p2->y - p1->y) / (p2->x - p1->x) * (X - p1->x) + p1->y; // y point where line p1->p2 intersects x=X
            if ((gsl_finite(y_intersect)) && (((X >= p1->x) && (X < p2->x)) || ((X <= p1->x) && (X > p2->x))) &&
                (((dir_y > 0) && (y_intersect > Y)) || ((dir_y < 0) && (y_intersect < Y)))) {
                n++;
            }
        } else {
            double x_intersect =
                    (p2->x - p1->x) / (p2->y - p1->y) * (Y - p1->y) + p1->x; // x point where line p1->p2 intersects y=Y
            if ((gsl_finite(x_intersect)) && (((Y >= p1->y) && (Y < p2->y)) || ((Y <= p1->y) && (Y > p2->y))) &&
                (((dir_x > 0) && (x_intersect > X)) || ((dir_x < 0) && (x_intersect < X)))) {
                n++;
            }
        }
    }
    unsigned char inside = (n % 2); // If were an even number of segments, we are outside. If there were an odd number, we are inside.
    return inside;
}

//! psPoint - Add a point to the postscript path which outlines a filled region
//! \param [in] fr - The filled region being drawn
//! \param [in] X - The horizontal position of the point, in postscript points
//! \param [in] Y - The vertical position of the point, in postscript points
//! \param [in,out] first_subpath - Flag indicating whether no subpath has been started yet; set to false
//! \param [in,out] first_point - Flag indicating whether this is the first point in its subpath; set to false
//! \param [in] linewidth - The line width with which the path is to be stroked, used to compute its bounding box

static void psPoint(FilledRegionHandle *fr, const double X, const double Y, unsigned char *first_subpath,
                    unsigned char *first_point, const double linewidth) {
    // Log progress in debugging mode
    if (DEBUG) {
        char msg[PPL_MSG_LENGTH];
        ppl_snprintfTrunc(msg, sizeof(msg), "Hopping to (%e,%e).", X, Y);
        ppl_log(&fr->x->c->errcontext, msg);
    }
    // Start a new path, if this is the first point
    if (*first_subpath) {
        fprintf(fr->x->epsbuffer, "newpath ");
        *first_subpath = 0;
    }
    // Move to the first point of each subpath, and draw lines to the others
    if (*first_point) {
        fprintf(fr->x->epsbuffer, "%.2f %.2f moveto\n", X, Y);
        *first_point = 0;
    } else {
        fprintf(fr->x->epsbuffer, "%.2f %.2f lineto\n", X, Y);
    }
    eps_core_BoundingBox(fr->x, X, Y, linewidth * EPS_DEFAULT_LINEWIDTH);
}

//! OutputPath - Write the postscript path of the clipped outline of a filled region
//! \param [in] fr - The handle of the filled region
//! \param [in] CrossPointList - The list of points where the outline crosses the edge of the graph
//! \param [in] nac - The number of points in <CrossPointList>
//! \param [in] EndText - The postscript command with which to end the path, e.g. "fill" or "stroke"
//! \param [in] linewidth - The line width with which to stroke the path

static void OutputPath(FilledRegionHandle *fr, FilledRegionAxisCrossing *CrossPointList, const int nac, char *EndText,
                       const double linewidth) {
    char msg[PPL_MSG_LENGTH];
    unsigned char first_point = 1, first_subpath = 1, fail = 0;
    int i, inew, j, k, dir_x, dir_y;
    listItem *lil = NULL;

    // Clear used flags
    for (i = 0; i < nac; i++) CrossPointList[i].used = 0;

    // Loop over multiple detached segments of filled region which may be caused by clipping it into pieces
    if (DEBUG) { ppl_log(&fr->x->c->errcontext, "New filled region."); }
    // Draw each separate piece of the region which lies within the graph, starting from a crossing point which has not
    // yet been used
    while (!fail) {
        // Find an axis crossing point which is unique
        int l = -1;
        // Prefer a crossing point which does not coincide with its neighbours
        for (i = 0; i < nac; i++) {
            if (CrossPointList[i].used) continue;
            l = i; // Fallback option; an unused crossing point
            j = (i + 1) % nac; // Next axis crossing
            k = (i + nac - 1) % nac; // Prev axis crossing
            if ((CrossPointList[i].x == CrossPointList[k].x) && (CrossPointList[i].y == CrossPointList[k].y)) continue;
            if ((CrossPointList[i].x == CrossPointList[j].x) && (CrossPointList[i].y == CrossPointList[j].y)) continue;
            break;
        }
        // Stop when all crossing points have been used
        if (l == -1) {
            if (DEBUG) { ppl_log(&fr->x->c->errcontext, "All axis-crossing points used."); }
            break;
        }
        i = l;

        // Debugging lines
        if (DEBUG) {
            ppl_log(&fr->x->c->errcontext, "New segment of filled region.");
            ppl_log(&fr->x->c->errcontext, "Summary of axis crossing points:");
            // Log each crossing point in debugging mode
            for (int i = 0; i < nac; i++) {
                ppl_snprintfTrunc(msg, sizeof(msg), "%d %s (%e,%e) face %d position %.4f sense %s %s", i,
                                  CrossPointList[i].used ? "[used]" : "      ", CrossPointList[i].x,
                                  CrossPointList[i].y, (int) CrossPointList[i].AxisFace, CrossPointList[i].AxisPos,
                                  (CrossPointList[i].sense == INCOMING) ? "incoming" : "outgoing",
                                  CrossPointList[i].singleton ? "[singleton]" : "");
                ppl_log(&fr->x->c->errcontext, msg);
            }
        }

        // Work out which face first axis crossing is on, and which direction we're moving going to next item in crossing point list
        unsigned char face = CrossPointList[i].AxisFace;
        // The direction in which the edge of the graph runs, anticlockwise, from this crossing point
        if (face == FACE_TOP) {
            dir_x = -1;
            dir_y = 0;
        } else if (face == FACE_LEFT) {
            dir_x = 0;
            dir_y = -1;
        } else if (face == FACE_BOTTOM) {
            dir_x = 1;
            dir_y = 0;
        } else {
            dir_x = 0;
            dir_y = 1;
        }
        unsigned char inside_ahead = TestPointInside(fr, CrossPointList[i].x, CrossPointList[i].y, dir_x, dir_y);

        // Decide whether we are proceeding clockwise or anticlockwise around the region we're going to fill
        unsigned char FillSide = inside_ahead;
        // Log the direction of travel in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Starting from %d; filling on %s.", i, FillSide ? "right" : "left");
            ppl_log(&fr->x->c->errcontext, msg);
        }

        // Follow the outline inside the graph to the next crossing, then along the edge of the graph to the next
        // crossing back in, until the piece is closed
        while (1) {
            CrossPointList[i].used = 1;
            if (CrossPointList[i].singleton) // Move along a line segment which streaks across canvas in one step
            {
                psPoint(fr, CrossPointList[i].x, CrossPointList[i].y, &first_subpath, &first_point,
                        linewidth); // Cut point where line enters canvas
                psPoint(fr, CrossPointList[i].x2, CrossPointList[i].y2, &first_subpath, &first_point,
                        linewidth); // Cut point where line leaves canvas
                if (((FilledRegionPoint *) CrossPointList[i].point->data)->FillSideFlip_prv % 2) FillSide = !FillSide;
                for (inew = 0; inew < nac; inew++)
                    if (CrossPointList[inew].id == CrossPointList[i].twin) break;
                // Report a missing twin crossing point
                if (inew >= nac) {
                    ppl_error(&fr->x->c->errcontext, ERR_INTERNAL, -1, -1, "Failure within FilledRegion");
                    fail = 1;
                    break;
                }
                // Log the twin in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Twin of %d is %d.", i, inew);
                    ppl_log(&fr->x->c->errcontext, msg);
                }
                i = inew;
                CrossPointList[i].used = 1;
            } else // Follow a path across the canvas, step by step
            {
                psPoint(fr, CrossPointList[i].x, CrossPointList[i].y, &first_subpath, &first_point,
                        linewidth); // Cut point where line enters canvas
                listItem *li = CrossPointList[i].point;
                unsigned char sense = CrossPointList[i].sense;
                // Follow the outline from the crossing, in the direction which leads inside the graph
                if (sense == OUTGOING) {
                    li = li->prev;
                    if (li == NULL) li = fr->points->last;
                }
                FilledRegionPoint *p = (FilledRegionPoint *) (li->data);
                // Draw each point inside the graph, flipping the fill side where the outline crosses itself
                while (p->inside) {
                    psPoint(fr, p->x, p->y, &first_subpath, &first_point, linewidth);
                    if ((sense == OUTGOING) && (p->FillSideFlip_fwd % 2)) FillSide = !FillSide;
                    else if ((sense == INCOMING) && (p->FillSideFlip_prv % 2)) FillSide = !FillSide;
                    lil = li;
                    // Step backwards or forwards along the outline
                    if (sense == OUTGOING) {
                        li = li->prev;
                        if (li == NULL) li = fr->points->last;
                    } // Moving against flow in which line was originally drawn
                    // Moving forwards
                    else {
                        li = li->next;
                        if (li == NULL) li = fr->points->first;
                    } // Moving with flow in which line was originally drawn
                    p = (FilledRegionPoint *) (li->data);
                }
                if ((sense == OUTGOING) && (p->FillSideFlip_fwd % 2)) FillSide = !FillSide;
                else if ((sense == INCOMING) && (p->FillSideFlip_prv % 2)) FillSide = !FillSide;
                for (i = 0; i < nac; i++)
                    if ((!CrossPointList[i].used) &&
                        ((CrossPointList[i].point == lil) || (CrossPointList[i].point == li)))
                        break;
                // Fail if no crossing point was found (in which case i == nac, past the end of the list)
                if (i >= nac) {
                    ppl_error(&fr->x->c->errcontext, ERR_INTERNAL, -1, -1, "Failure within FilledRegion");
                    fail = 1;
                    break;
                }
                psPoint(fr, CrossPointList[i].x, CrossPointList[i].y, &first_subpath, &first_point,
                        linewidth); // Cut point where line leaves canvas
                // Log the crossing point in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Departed canvas at crossing point %d.", i);
                    ppl_log(&fr->x->c->errcontext, msg);
                }
                CrossPointList[i].used = 1;
            }

            // Skirt along edge of clip region to find next entry point
            for (k = i; ((CrossPointList[i].x == CrossPointList[k].x) && (CrossPointList[i].y == CrossPointList[k].y));
                 k = (k + 1) % nac)
                // Use any other unused crossing point at the same place, flipping the fill side
                if (!CrossPointList[k].used) {
                    i = k;
                    FillSide = !FillSide;
                    continue;
                }
            for (k = i; ((CrossPointList[i].x == CrossPointList[k].x) && (CrossPointList[i].y == CrossPointList[k].y));
                 k = (k + nac - 1) % nac)
                // Use any other unused crossing point at the same place, flipping the fill side
                if (!CrossPointList[k].used) {
                    i = k;
                    FillSide = !FillSide;
                    continue;
                }
            if (FillSide) j = (i + nac - 1) % nac; // Move anticlockwise around path
            else j = (i + 1) % nac; // Move clockwise around path
            unsigned char CurrentFace = CrossPointList[i].AxisFace;
            // Log the move along the edge in debugging mode
            if (DEBUG) {
                ppl_snprintfTrunc(
                        msg, sizeof(msg),
                        "Move along edge of canvas from %d to %d; filling on %s; moving from axis face %d to %d.", i, j,
                        FillSide ? "right" : "left", (int) CrossPointList[i].AxisFace,
                        (int) CrossPointList[j].AxisFace);
                ppl_log(&fr->x->c->errcontext, msg);
            }
            while (CurrentFace !=
                   CrossPointList[j].AxisFace) // If we've changed side of clip region, add corner to path
            {
                double xap, yap;
                const double xaps[6] = {9, 1, 0, 0, 1, 1};
                const double yaps[6] = {9, 1, 1, 0, 0, 1};
                // Add the corner at the end of this face of the graph, in the direction of travel
                if (!FillSide) {
                    xap = xaps[CurrentFace + 1];
                    yap = yaps[CurrentFace + 1];
                    CurrentFace = (CurrentFace % 4) + 1;
                } else {
                    xap = xaps[CurrentFace];
                    yap = yaps[CurrentFace];
                    CurrentFace = ((CurrentFace + 2) % 4) + 1;
                }
                // Log the corner in debugging mode
                if (DEBUG) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Corner (%e,%e)", xap, yap);
                    ppl_log(&fr->x->c->errcontext, msg);
                }
                // Place the corner on the page, in two or three dimensions
                if (!fr->ThreeDim) {
                    psPoint(fr, fr->origin_x + fr->width * xap, fr->origin_y + fr->height * yap, &first_subpath,
                            &first_point, linewidth);
                } else {
                    psPoint(fr,
                            fr->origin_x + fr->width / 2 + (xap - 0.5) * fr->width * cos(fr->sg->XYview.real) +
                                    (yap - 0.5) * fr->height * sin(fr->sg->XYview.real),
                            fr->origin_y + fr->height / 2 -
                                    (xap - 0.5) * fr->width * cos(fr->sg->YZview.real) * sin(fr->sg->XYview.real) +
                                    (yap - 0.5) * fr->height * cos(fr->sg->YZview.real) * cos(fr->sg->XYview.real),
                            &first_subpath, &first_point, linewidth);
                }
            }
            i = j;
            if (CrossPointList[i].used) break;
        }
        if (!first_point) { fprintf(fr->x->epsbuffer, "closepath\n"); }
        first_point = 1;
        if (DEBUG) { ppl_log(&fr->x->c->errcontext, "End segment."); }
    }
    if (DEBUG) { ppl_log(&fr->x->c->errcontext, "End filled region."); }
    if (!first_point) { fprintf(fr->x->epsbuffer, "closepath\n"); }
    fprintf(fr->x->epsbuffer, "%s\n", EndText);
    return;
}

//! FilledRegion_Finish - Finish drawing a filled region, clipping its outline to the edges of the graph and writing it
//! to postscript
//! \param [in] X - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] fr - The handle of the filled region
//! \param [in] linetype - The line type (dash pattern)
//! \param [in] linewidth - The line width, as a multiple of the default line width
//! \param [in] StrokeOutline - If true, also draw the outline of the region

void FilledRegion_Finish(const EPSComm *X, FilledRegionHandle *fr, const int linetype, const double linewidth,
                         const unsigned char StrokeOutline) {
    unsigned char f1, f2, first_point = 1, first_subpath = 1;
    int i, j, k, kn, NCrossings, i1, i2;
    double cx1, cy1, cz1, cx2, cy2, cz2, ap1, ap2;
    FilledRegionPoint *p;
    const EPSComm *x = fr->x; // Makes if (eps_core_isVisible(x)) macro work

    // Variables for finding where the outline crosses the edges of the graph, and where it crosses itself
    int l = fr->points->length;
    listItem *li = fr->points->first;
    if (l < 2) return; // No points on outline to stroke
    if (!fr->EverInside) return; // Path never ventures within clip region
    eps_core_WritePSColor(fr->x);

    // Path never goes outside the clip region
    if (fr->Naxiscrossings < 1) {
        if (DEBUG) { ppl_log(&fr->x->c->errcontext, "New filled region never leaves clip region."); }
        if (DEBUG) { ppl_log(&fr->x->c->errcontext, "1. Fill it."); }
        // If the outline never leaves the graph, simply fill it, unless the color is invisible
        if (eps_core_isVisible(x)) {
            // Draw each point of the outline
            for (i = 0; i < l; i++) {
                p = (FilledRegionPoint *) (li->data);
                psPoint(fr, p->x, p->y, &first_subpath, &first_point, linewidth);
                li = li->next;
            }
            fprintf(fr->x->epsbuffer, "closepath eofill\n");
        }
        eps_core_SwitchFrom_FillColor(fr->x, 1);
        if (!StrokeOutline) return;
        eps_core_SetLinewidth(fr->x, linewidth, linetype, 0.0);
        li = fr->points->first;
        first_point = first_subpath = 1;
        if (DEBUG) { ppl_log(&fr->x->c->errcontext, "2. Stroke its outline."); }
        // Stroke the outline, unless the color is invisible
        if (eps_core_isVisible(x)) {
            // Draw each point of the outline
            for (i = 0; i < l; i++) {
                p = (FilledRegionPoint *) (li->data);
                psPoint(fr, p->x, p->y, &first_subpath, &first_point, linewidth);
                li = li->next;
            }
            fprintf(fr->x->epsbuffer, "closepath stroke\n");
        }
        return;
    }

    // Close path to get potential final axis crossing
    p = (FilledRegionPoint *) (li->data);
    FilledRegion_Point(fr->x, fr, p->xa, p->ya); // Add first point to end of point list
    l = fr->points->length;
    double lastx = p->x;
    double lasty = p->y;
    double lastxap = p->xap;
    double lastyap = p->yap;
    FilledRegionPoint *last_p = p;
    li = li->next;

    // Malloc a structure to hold the positions of all of the points where we cross the boundary of the clip region
    FilledRegionAxisCrossing *CrossPointList =
            (FilledRegionAxisCrossing *) ppl_memAlloc(fr->Naxiscrossings * 2 * sizeof(FilledRegionAxisCrossing));
    if (CrossPointList == NULL) return;
    // Find where each segment of the outline crosses the edges of the graph, and where it crosses earlier segments
    for (i = 1, j = 0; i < l; i++) {
        p = (FilledRegionPoint *) (li->data);
        LineDraw_FindCrossingPoints(fr->x, lastx, lasty, 0, lastxap, lastyap, 0.5, p->x, p->y, 0, p->xap, p->yap, 0.5,
                                    &i1, &i2, &cx1, &cy1, &cz1, &cx2, &cy2, &cz2, &f1, &ap1, &f2, &ap2, &NCrossings);

        // Check for whether line crosses a previous line segment; if so, flip fill side
        double ct_y_m1 = (cy2 - cy1) / (cx2 - cx1);
        double ct_y_c1 = cy1 - ct_y_m1 * cx1;
        double ct_x_m1 = (cx2 - cx1) / (cy2 - cy1);
        double ct_x_c1 = cx1 - ct_x_m1 * cy1;
        listItem *ct_li = fr->points->first;
        FilledRegionPoint *ct_pB = (FilledRegionPoint *) (ct_li->data);
        ct_li = ct_li->next;
        // Test each earlier segment for an intersection with this one
        for (k = 1, kn = 0; k < (i - 1); k++) {
            FilledRegionPoint *ct_pA = ct_pB;
            ct_pB = (FilledRegionPoint *) (ct_li->data);
            ct_li = ct_li->next;
            double ct_y_m2 = (ct_pB->y - ct_pA->y) / (ct_pB->x - ct_pA->x);
            double ct_y_c2 = ct_pA->y - ct_y_m2 * ct_pA->x;
            double ct_x_m2 = (ct_pB->x - ct_pA->x) / (ct_pB->y - ct_pA->y);
            double ct_x_c2 = ct_pA->x - ct_x_m2 * ct_pA->y;
            double ct_xi = (ct_y_c2 - ct_y_c1) / (ct_y_m1 - ct_y_m2);
            double ct_yi = (ct_x_c2 - ct_x_c1) / (ct_x_m1 - ct_x_m2);
            if ((gsl_finite(ct_xi)) && (((ct_xi >= cx1) && (ct_xi < cx2)) || ((ct_xi <= cx1) && (ct_xi > cx2))) &&
                (((ct_xi >= ct_pA->x) && (ct_xi < ct_pB->x)) || ((ct_xi <= ct_pA->x) && (ct_xi > ct_pB->x)))) {
                kn++;
                ct_pB->FillSideFlip_prv++;
                ct_pB->FillSideFlip_fwd++;
            } else if ((gsl_finite(ct_yi)) &&
                       (((ct_yi >= cy1) && (ct_yi < cy2)) || ((ct_yi <= cy1) && (ct_yi > cy2))) &&
                       (((ct_yi >= ct_pA->y) && (ct_yi < ct_pB->y)) || ((ct_yi <= ct_pA->y) && (ct_yi > ct_pB->y)))) {
                kn++;
                ct_pB->FillSideFlip_prv++;
                ct_pB->FillSideFlip_fwd++;
            }
        }
        p->FillSideFlip_prv = kn;
        last_p->FillSideFlip_fwd = kn;


        if (((!i1) || (!i2)) && (NCrossings > 0)) // We have crossed boundary
        {
            // Record where the segment enters or leaves the graph
            if (!i1) {
                CrossPointList[j].x = cx1;
                CrossPointList[j].y = cy1;
                CrossPointList[j].AxisFace = f1;
                CrossPointList[j].AxisPos = ap1;
            } else {
                CrossPointList[j].x = cx2;
                CrossPointList[j].y = cy2;
                CrossPointList[j].AxisFace = f2;
                CrossPointList[j].AxisPos = ap2;
            }
            // A segment which crosses the whole graph enters and leaves it; it is recorded as a pair of twinned
            // crossing points
            if ((!i1) && (!i2)) {
                CrossPointList[j].x2 = cx2;
                CrossPointList[j].y2 = cy2;
            }
            CrossPointList[j].id = j;
            CrossPointList[j].singleton = ((!i1) && (!i2));
            CrossPointList[j].twin = j + 1;
            CrossPointList[j].sense = (i1) ? OUTGOING : INCOMING;
            CrossPointList[j].point = li;
            CrossPointList[j].used = 0;
            j++;
        }
        // Record the second crossing point of a segment which crosses the whole graph
        if (((!i1) && (!i2)) && (NCrossings > 0)) {
            CrossPointList[j] = CrossPointList[j - 1];
            CrossPointList[j].x = cx2;
            CrossPointList[j].y = cy2;
            CrossPointList[j].x2 = cx1;
            CrossPointList[j].y2 = cy1;
            CrossPointList[j].id = j;
            CrossPointList[j].AxisFace = f2;
            CrossPointList[j].AxisPos = ap2;
            CrossPointList[j].sense = OUTGOING;
            CrossPointList[j].twin = j - 1;
            j++;
        }
        lastx = p->x;
        lasty = p->y;
        lastxap = p->xap;
        lastyap = p->yap;
        last_p = p;
        li = li->next;
    }

    // Sort list into order around perimeter of clipping area
    qsort((void *) CrossPointList, j, sizeof(FilledRegionAxisCrossing), &frac_sorter);

    // Output path
    if (DEBUG) { ppl_log(&fr->x->c->errcontext, "New filled region which crosses edge of clip region."); }
    if (DEBUG) { ppl_log(&fr->x->c->errcontext, "1. Fill it."); }
    if (eps_core_isVisible(x)) OutputPath(fr, CrossPointList, j, "eofill", linewidth);
    eps_core_SwitchFrom_FillColor(fr->x, 1);
    if (!StrokeOutline) return;
    eps_core_SetLinewidth(fr->x, linewidth, linetype, 0.0);
    li = fr->points->first;
    if (DEBUG) { ppl_log(&fr->x->c->errcontext, "2. Stroke its outline."); }
    if (eps_core_isVisible(x)) OutputPath(fr, CrossPointList, j, "stroke", linewidth);
    return;
}
