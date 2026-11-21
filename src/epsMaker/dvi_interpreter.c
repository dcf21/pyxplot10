// dvi_interpreter.c
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

// Interprets the operators of a dvi file, as read by dvi_read.c, keeping track of the current position, spacing values
// and font, and writes postscript which draws the same text: runs of characters become postscript show commands in Type
// 1 fonts loaded by dvi_font.c, rules become filled rectangles, and color specials from LaTeX's color package become
// color changes. Each page of the dvi file becomes a separate postscript page, with a bounding box, and a box measuring
// the text's cap height, which canvasDraw.c uses to align the text.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/list.h"
#include "coreUtils/memAlloc.h"
#include "stringTools/strConstants.h"
#include "epsMaker/dvi_interpreter.h"
#include "epsMaker/dvi_font.h"
#include "epsMaker/dvi_read.h"

// Table of the operator functions to allow quick lookup without a long if ... else if ... statement
int (*dviOpTable[58])(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op);

//! makeDviOpTable - Build the table mapping each DVI opcode onto the function which implements it, so that operators
//! can be looked up quickly
//! \param [in] ec - The error-reporting context

void makeDviOpTable(const pplerr_context *ec) {
    // Each opcode maps onto the function which implements it; a NULL entry means the opcode is handled elsewhere
    dviOpTable[0] = dviInOpSet1234;
    dviOpTable[1] = dviInOpSet1234;
    dviOpTable[2] = dviInOpSet1234;
    dviOpTable[3] = dviInOpSet1234;
    dviOpTable[4] = dviInOpSetRule;
    dviOpTable[5] = dviInOpPut1234;
    dviOpTable[6] = dviInOpPut1234;
    dviOpTable[7] = dviInOpPut1234;
    dviOpTable[8] = dviInOpPut1234;
    dviOpTable[9] = dviInOpPutRule;
    dviOpTable[10] = dviInOpNop;
    dviOpTable[11] = dviInOpBop;
    dviOpTable[12] = dviInOpEop;
    dviOpTable[13] = dviInOpPush;
    dviOpTable[14] = dviInOpPop;
    dviOpTable[15] = dviInOpRight1234;
    dviOpTable[16] = dviInOpRight1234;
    dviOpTable[17] = dviInOpRight1234;
    dviOpTable[18] = dviInOpRight1234;
    dviOpTable[19] = dviInOpW0;
    dviOpTable[20] = dviInOpW1234;
    dviOpTable[21] = dviInOpW1234;
    dviOpTable[22] = dviInOpW1234;
    dviOpTable[23] = dviInOpW1234;
    dviOpTable[24] = dviInOpX0;
    dviOpTable[25] = dviInOpX1234;
    dviOpTable[26] = dviInOpX1234;
    dviOpTable[27] = dviInOpX1234;
    dviOpTable[28] = dviInOpX1234;
    dviOpTable[29] = dviInOpDown1234;
    dviOpTable[30] = dviInOpDown1234;
    dviOpTable[31] = dviInOpDown1234;
    dviOpTable[32] = dviInOpDown1234;
    dviOpTable[33] = dviInOpY0;
    dviOpTable[34] = dviInOpY1234;
    dviOpTable[35] = dviInOpY1234;
    dviOpTable[36] = dviInOpY1234;
    dviOpTable[37] = dviInOpY1234;
    dviOpTable[38] = dviInOpZ0;
    dviOpTable[39] = dviInOpZ1234;
    dviOpTable[40] = dviInOpZ1234;
    dviOpTable[41] = dviInOpZ1234;
    dviOpTable[42] = dviInOpZ1234;
    // Big hole here where we ignore FONTi
    dviOpTable[43] = dviInOpFnt1234;
    dviOpTable[44] = dviInOpFnt1234;
    dviOpTable[45] = dviInOpFnt1234;
    dviOpTable[46] = dviInOpFnt1234;
    dviOpTable[47] = dviInOpSpecial1234;
    dviOpTable[48] = dviInOpSpecial1234;
    dviOpTable[49] = dviInOpSpecial1234;
    dviOpTable[50] = dviInOpSpecial1234;
    dviOpTable[51] = dviInOpFntdef1234;
    dviOpTable[52] = dviInOpFntdef1234;
    dviOpTable[53] = dviInOpFntdef1234;
    dviOpTable[54] = dviInOpFntdef1234;
    dviOpTable[55] = dviInOpPre;
    dviOpTable[56] = dviInOpPost;
    dviOpTable[57] = dviInOpPostPost;
    return;
}

//! dviInterpretOperator - Interpret a single DVI operator, by calling the function which implements it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInterpretOperator(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    // Variables for finding the function which implements the operator
    int (*func)(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) = NULL;
    int i = 0;
    int err;
    char errStr[SSTR_LENGTH];

    // Deal with the processing of DVI extensions (DIV_XXX/SPECIAL)
    if (interp->special > 0) {
        // While a special command's argument is being read, characters are accumulated in it; any other operator ends
        // it
        if ((op->op <= DVI_CHARMAX) && (interp->specialLen-- > 0)) {
            return dviSpecialChar(ec, interp, op);
        } else {
            // The following function turns the special flag off, and hence op is evaluated below
            if ((err = dviSpecialImplement(ec, interp)) != 0) return err;
        }
    }

    // This if statement extends the lookup table of operator functions
    if (op->op <= DVI_CHARMAX) {
        func = dviInOpChar;
    } else if (op->op < DVI_FNTNUMMIN) {
        i = op->op - DVI_CHARMAX - 1;
        func = (dviOpTable[i]);
    } else if (op->op < DVI_FNTNUMMAX) {
        func = dviInOpFnt;
    } else if (op->op <= DVI_POSTPOST) {
        i = op->op - DVI_CHARMAX - 1 - (DVI_FNTNUMMAX - DVI_FNTNUMMIN + 1);
        func = (dviOpTable[i]);
    } else {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "dvi interpreter found an illegal operator");
        return DVIE_CORRUPT;
    }
    // Report an opcode with no handler
    if (func == NULL) {
        snprintf(errStr, SSTR_LENGTH, "Failed to find handler for dvi operator %d i=%d", op->op, i);
        ppl_error(ec, ERR_INTERNAL, -1, -1, errStr);
        return DVIE_CORRUPT;
    }

    // If we are not typesetting a character and moving right, check if we need to set accumulated text
    if (op->op > DVI_SET1234 + 3 && interp->currentString != NULL)
        if ((err = dviTypeset(ec, interp)) != 0) return err;

    // Call the function to interpret the operator
    return (*func)(ec, interp, op);
}

// ----------------------------------
// Functions that implement operators
// ----------------------------------

//! dviNonAsciiChar - Typeset a character which cannot be written directly in a postscript string, using its octal
//! escape code
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] c - The character code
//! \param [in] move - DVI_YES to move right by the width of the character after typesetting it, or DVI_NO to leave the
//! position unchanged
//! \return Zero on success, or a DVIE_* error code

int dviNonAsciiChar(pplerr_context *ec, dviInterpreterState *interp, int c, const char move) {
    // Variables for typesetting the character
    char msg[PPL_MSG_LENGTH];
    char s[64];
    int err;
    dviStackState *postPos, *dviPos; // Current positions in dvi and postscript code
    double size[4];

    // Move to the current position before typesetting, if the postscript position differs from it
    postPos = interp->output->currentPosition;
    dviPos = interp->state;

    // First check if we need to move before typesetting
    if (postPos == NULL) {
        dviPostscriptMoveto(ec, interp);
        interp->output->currentPosition = dviCloneInterpState(ec, dviPos);
    } else if (postPos->h != dviPos->h || postPos->v != dviPos->v) {
        dviPostscriptMoveto(ec, interp);
    }

    // Update bounding box
    dviGetCharSize(ec, interp, (char) c, size);
    const double width = size[0] / interp->scale; // Convert back into dvi units
    const double height = size[1] / interp->scale;
    const double depth = size[2] / interp->scale;
    const double italic = size[3] / interp->scale;
    // Log the size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "DVI: width of glyph %g height of glyph %g", width, height);
        ppl_log(ec, msg);
    }
    if ((err = dviUpdateBoundingBox(ec, interp, width + italic, height, depth)) != 0) return err;

    // Count the number of characters to write to the ps string
    snprintf(s, 64, "(\\%o) show\n", c);

    // Send the string off to the postscript routine and clean up memory
    if ((err = dviPostscriptAppend(ec, interp, s)) != 0) return err;
    //free(interp->currentString);
    interp->currentString = NULL;
    interp->currentStrlen = 0;

    // Adjust the current position
    if (move == DVI_YES) {
        interp->state->h += width;
        interp->output->currentPosition->h += width;
    }
    return 0;
}

//! dviInOpChar - Implement the DVI_CHAR operators (opcodes 0-127), which typeset the character with that code and move
//! right by its width
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpChar(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    // The character to typeset
    int charToTypeset = op->op;
    int err;

    // Typeset non-printable characters separately
    if ((charToTypeset < 48) || ((charToTypeset > 57) && (charToTypeset < 65)) ||
        ((charToTypeset > 90) && (charToTypeset < 97)) || (charToTypeset > 126)) {
        // Clear the queue if there's anything on it
        if (interp->currentString != NULL)
            if ((err = dviTypeset(ec, interp)) != 0) return err;
        if ((err = dviNonAsciiChar(ec, interp, charToTypeset, DVI_YES)) != 0) return err;
        return 0;
    }

    // See if we have anywhere to put the character
    if (interp->currentString == NULL) {
        interp->currentString = (char *) ppl_memAlloc(SSTR_LENGTH);
        // Failure to allocate memory is reported as an error
        if (interp->currentString == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return DVIE_MEMORY;
        }
        *(interp->currentString) = '\0';
        interp->currentStrlen = SSTR_LENGTH;
    } else if (strlen(interp->currentString) + 3 >= interp->currentStrlen) // If the string is full, start a new one;
                                                                           // up to two characters and a \0 are added
    {
        if ((err = dviTypeset(ec, interp)) != 0) return err;
        interp->currentString = (char *) ppl_memAlloc(SSTR_LENGTH);
        // Failure to allocate memory is reported as an error
        if (interp->currentString == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return DVIE_MEMORY;
        }
        *(interp->currentString) = '\0';
        interp->currentStrlen = SSTR_LENGTH;
    }
    // Write the character to the string
    char *s = interp->currentString + strlen(interp->currentString); // s now points to the \0
    if (charToTypeset == 40) snprintf(s, 3, "%s", "\\(");
    else if (charToTypeset == 41) snprintf(s, 3, "%s", "\\)");
    else snprintf(s, 2, "%c", charToTypeset);
    return 0;
}

//! dviInOpSet1234 - Implement the DVI_SET1234 operator, which typesets a character with a code given explicitly in the
//! operator, and moves right by its width
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpSet1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    int err;
    // Typeset any characters queued up before this one (e.g. "Gr" before the "\366" of "Gr\366"); otherwise
    // dviNonAsciiChar() discards them
    if (interp->currentString != NULL)
        if ((err = dviTypeset(ec, interp)) != 0) return err;
    return dviNonAsciiChar(ec, interp, op->ul[0], DVI_YES);
}

//! dviInOpSetRule - Implement the DVI_SETRULE operator, which draws a filled rectangle (rule) and moves right by its
//! width
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpSetRule(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    int err = 0;

    // Don't set a rule if movements are -ve
    if (op->sl[0] < 0 || op->sl[1] < 0) {
        if (DEBUG) ppl_log(ec, "DVI: silent rule");
        interp->state->h += op->sl[1];
        if ((err = dviPostscriptMoveto(ec, interp)) != 0) return err;
    } else {
        if ((err = dviUpdateBoundingBox(ec, interp, (int) op->sl[1], (int) op->sl[0], 0.0)) != 0) return err;
        if ((err = dviPostscriptMoveto(ec, interp)) != 0) return err;
        interp->state->v -= op->sl[0];
        if ((err = dviPostscriptLineto(ec, interp)) != 0) return err;
        interp->state->h += op->sl[1];
        if ((err = dviPostscriptLineto(ec, interp)) != 0) return err;
        interp->state->v += op->sl[0];
        if ((err = dviPostscriptLineto(ec, interp)) != 0) return err;
        if ((err = dviPostscriptClosepathFill(ec, interp)) != 0) return err;
    }
    return err;
}

//! dviInOpPut1234 - Implement the DVI_PUT1234 operator, which typesets a character without moving right
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpPut1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    return dviNonAsciiChar(ec, interp, op->ul[0], DVI_NO);
}

//! dviInOpPutRule - Implement the DVI_PUTRULE operator, which draws a filled rectangle (rule) without moving right
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpPutRule(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    int err = 0;

    // Don't set a rule if movements are -ve
    if (op->sl[0] < 0 || op->sl[1] < 0) {
        if (DEBUG) ppl_log(ec, "DVI: silent rule");
    } else {
        if ((err = dviUpdateBoundingBox(ec, interp, (int) op->sl[1], (int) op->sl[0], 0.0)) != 0) return err;
        if ((err = dviPostscriptMoveto(ec, interp)) != 0) return err;
        interp->state->v -= op->sl[0];
        if ((err = dviPostscriptLineto(ec, interp)) != 0) return err;
        interp->state->h += op->sl[1];
        if ((err = dviPostscriptLineto(ec, interp)) != 0) return err;
        interp->state->v += op->sl[0];
        if ((err = dviPostscriptLineto(ec, interp)) != 0) return err;
        if ((err = dviPostscriptClosepathFill(ec, interp)) != 0) return err;
        interp->state->h -= op->sl[1];
    }
    return err;
}

//! dviInOpNop - Implement the DVI_NOP operator, which does nothing
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpNop(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    return 0;
}

//! dviInOpBop - Implement the DVI_BOP operator, which begins a new page, starting a new postscript page and resetting
//! the position and stack
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpBop(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    // Generate a new page
    postscriptPage *p = dviNewPostscriptPage(ec);
    if (p == NULL) return DVIE_MEMORY;
    ppl_listAppend(interp->output->pages, (void *) p);
    interp->output->currentPage = p;
    interp->output->Npages++;

    // Check that the stack is empty
    if (ppl_listLen(interp->stack) > 0) {
        ppl_warning(ec, ERR_INTERNAL, "malformed DVI file: stack not empty at start of new page");
        interp->stack = ppl_listInit(0);
    }

    // There should not be a string in progress on the stack
    if (interp->currentStrlen != 0) {
        ppl_warning(ec, ERR_INTERNAL, "error in DVI interpreter: string on stack at newpage");
        interp->currentStrlen = 0;
        //free(interp->currentString);
        interp->currentString = NULL;
    }

    // Set the default state values
    interp->state->h = 0;
    interp->state->v = 0;
    interp->state->w = 0;
    interp->state->x = 0;
    interp->state->y = 0;
    interp->state->z = 0;
    // Leave f as it is undefined after a new page
    return 0;
}

//! dviInOpEop - Implement the DVI_EOP operator, which ends a page, finishing its postscript and recording its bounding
//! box
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpEop(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    // Variables for the page's bounding box
    char msg[PPL_MSG_LENGTH];

    // Set appropriate postscript bounding box from dvi bb
    // left bottom right top
    double *bb = interp->boundingBox;
    // If we have not typeset anything then the bounding box will be empty.
    if (bb == NULL) {
        bb = (double *) ppl_memAlloc(4 * sizeof(double));
        // Failure to allocate memory is reported as an error
        if (bb == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return DVIE_MEMORY;
        }
        bb[0] = interp->state->h;
        bb[1] = interp->state->v;
        bb[2] = interp->state->h;
        bb[3] = interp->state->v;
    }

    // Convert bounding box from DVI to PS units
    bb[0] *= interp->scale;
    bb[1] = 765 - bb[1] * interp->scale;
    bb[2] *= interp->scale;
    bb[3] = 765 - bb[3] * interp->scale;

    // Move pointer to postscript
    interp->output->currentPage->boundingBox = bb;
    interp->boundingBox = NULL;

    // Log the bounding box in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Postscript page: bounding box %f %f %f %f", bb[0], bb[1], bb[2], bb[3]);
        ppl_log(ec, msg);
    }

    // Now repeat for text size box
    bb = interp->textSizeBox;
    // If we have not typeset anything then the box will be empty.
    if (bb == NULL) {
        bb = (double *) ppl_memAlloc(4 * sizeof(double));
        // Failure to allocate memory is reported as an error
        if (bb == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return DVIE_MEMORY;
        }
        bb[0] = interp->state->h;
        bb[1] = interp->state->v;
        bb[2] = interp->state->h;
        bb[3] = interp->state->v;
    }

    // Convert the box from DVI to postscript units
    bb[0] *= interp->scale;
    bb[1] = 765 - bb[1] * interp->scale;
    bb[2] *= interp->scale;
    bb[3] = 765 - bb[3] * interp->scale;

    // Move pointer to postscript
    interp->output->currentPage->textSizeBox = bb;
    interp->textSizeBox = NULL;

    // Log the box in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Postscript page: text size box %f %f %f %f", bb[0], bb[1], bb[2], bb[3]);
        ppl_log(ec, msg);
    }

    // Done
    return 0;
}

//! dviInOpPush - Implement the DVI_PUSH operator, which pushes the current position and spacing values onto the stack
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpPush(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    // Push the current state on to the stack
    const dviStackState *new = dviCloneInterpState(ec, interp->state);
    if (new == NULL) return DVIE_MEMORY;
    ppl_listAppend(interp->stack, (void *) new);
    return 0;
}

//! dviInOpPop - Implement the DVI_POP operator, which pops the position and spacing values from the stack
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpPop(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    // Pop the previous state off the stack
    dviStackState *new = (dviStackState *) ppl_listPop(interp->stack);
    // Report a pop from an empty stack
    if (new == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "corrupt dvi file -- attempt to pop off empty stack");
        return 1;
    }
    interp->state = new;

    // Unset the special flag
    interp->special = 0;
    return 0;
}

//! dviInOpRight1234 - Implement the DVI_RIGHT1234 operator, which moves right by a specified distance
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpRight1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->h += op->sl[0];
    return 0;
}

//! dviInOpW0 - Implement the DVI_W0 operator, which moves right by the current w spacing
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpW0(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->h += interp->state->w;
    return 0;
}

//! dviInOpW1234 - Implement the DVI_W1234 operator, which sets the w spacing, and moves right by it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpW1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->w = op->sl[0];
    interp->state->h += interp->state->w;
    return 0;
}

//! dviInOpX0 - Implement the DVI_X0 operator, which moves right by the current x spacing
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpX0(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->h += interp->state->x;
    return 0;
}

//! dviInOpX1234 - Implement the DVI_X1234 operator, which sets the x spacing, and moves right by it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpX1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->x = op->sl[0];
    interp->state->h += interp->state->x;
    return 0;
}

//! dviInOpDown1234 - Implement the DVI_DOWN1234 operator, which moves down by a specified distance
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpDown1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->v += op->sl[0];
    return 0;
}

//! dviInOpY0 - Implement the DVI_Y0 operator, which moves down by the current y spacing
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpY0(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->v += interp->state->y;
    return 0;
}

//! dviInOpY1234 - Implement the DVI_Y1234 operator, which sets the y spacing, and moves down by it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpY1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->y = op->sl[0];
    interp->state->v += interp->state->y;
    return 0;
}

//! dviInOpZ0 - Implement the DVI_Z0 operator, which moves down by the current z spacing
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpZ0(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->v += interp->state->z;
    return 0;
}

//! dviInOpZ1234 - Implement the DVI_Z1234 operator, which sets the z spacing, and moves down by it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpZ1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    interp->state->z = op->sl[0];
    interp->state->v += interp->state->z;
    return 0;
}

//! dviInOpFnt - Implement the DVI_FNTNUM operators (opcodes 171-234), which select font number 0-63, encoded in the
//! opcode
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpFnt(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    const int fn = op->op - DVI_FNTNUMMIN;
    return dviChngFnt(ec, interp, fn);
}

//! dviInOpFnt1234 - Implement the DVI_FNT1234 operator, which selects a font whose number is given explicitly
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpFnt1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    dviChngFnt(ec, interp, op->ul[0]);
    return 0;
}

//! dviInOpSpecial1234 - Implement the DVI_SPECIAL1234 operator, which passes a special command (\special{}) to the
//! interpreter, e.g. to change color
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpSpecial1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    char msg[PPL_MSG_LENGTH];
    const int spesh = op->op - DVI_SPECIAL1234 + 1;
    interp->special = spesh;
    interp->specialLen = op->ul[0];
    interp->spString = (char *) ppl_memAlloc(SSTR_LENGTH);
    // Failure to allocate memory is reported as an error
    if (interp->spString == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    *(interp->spString) = '\0';
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "DVI: dvi special: %d %lu %d", spesh, op->ul[0],
                          (int) strlen(interp->spString));
        ppl_log(ec, msg);
    }
    // NOP
    return 0;
}

//! dviInOpFntdef1234 - Implement the DVI_FNTDEF1234 operator, which defines a font, loading its metrics so that it can
//! later be selected
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpFntdef1234(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    // XXX Should perhaps check that we're not re-defining an existing font here
    // XXX Could check that we're not after POST and hence that we need to do this

    // Allocate a record for the font
    dviFontDetails *font = (dviFontDetails *) ppl_memAlloc(sizeof(dviFontDetails));
    // Failure to allocate memory is reported as an error
    if (font == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    memset(font, 0, sizeof(dviFontDetails));
    ppl_listAppend(interp->fonts, (void *) font);

    // Populate with information from operator
    font->number = op->ul[0];
    font->area = (char *) ppl_memAlloc(op->ul[4] + 1);
    // Failure to allocate memory is reported as an error
    if (font->area == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    for (int i = 0; i < op->ul[4]; i++) font->area[i] = op->s[0][i];
    font->area[op->ul[4]] = '\0';
    font->name = (char *) ppl_memAlloc(op->ul[5] + 1);
    // Failure to allocate memory is reported as an error
    if (font->name == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    for (int i = 0; i < op->ul[5]; i++) font->name[i] = op->s[0][op->ul[4] + i];
    font->name[op->ul[5]] = '\0';
    font->useSize = op->ul[2];
    font->desSize = op->ul[3];

    // parse the TFM file for useful data
    return dviGetTFM(ec, font);
}

//! dviInOpPre - Implement the DVI_PRE operator, which is the preamble of the DVI file, which sets the units of
//! measurement and magnification
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpPre(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    char msg[PPL_MSG_LENGTH];
    unsigned long i = op->ul[0];
    unsigned long num = op->ul[1];
    unsigned long den = op->ul[2];
    unsigned long mag = op->ul[3];
    // Only version 2 of the dvi format is supported
    if (i != 2) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Error interpreting dvi file: not dvi version 2");
        return 1;
    }
    // Convert mag, num and den into points (for ps)
    interp->scale = (double) mag / 1000.0 * (double) num / (double) den / 1.0e3 * 72.0 / 254;
    // Log the scale in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "DVI: Scale %g V=%lu num=%lu den=%lu mag=%lu", interp->scale, i, num, den,
                          mag);
        ppl_log(ec, msg);
    }
    return 0;
}

//! dviInOpPost - Implement the DVI_POST operator, which begins the postamble of the DVI file, and is ignored
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpPost(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    return 0;
}

//! dviInOpPostPost - Implement the DVI_POSTPOST operator, which ends the postamble of the DVI file, and is ignored
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator to process
//! \return Zero on success, or a DVIE_* error code

int dviInOpPostPost(pplerr_context *ec, dviInterpreterState *interp, DVIOperator *op) {
    return 0;
}

// ----------------------------------------------------------
// Functions for implementing special operators (DVI_SPECIAL)
// ----------------------------------------------------------

//! dviSpecialChar - Accumulate a character of the argument of a special command into the special-command buffer
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] op - The DVI operator containing the character
//! \return Zero on success, or a DVIE_* error code

int dviSpecialChar(const pplerr_context *ec, const dviInterpreterState *interp, const DVIOperator *op) {
    char *s = interp->spString + strlen(interp->spString);
    snprintf(s, SSTR_LENGTH, "%c", op->op);
    return 0;
}

//! dviSpecialImplement - Implement a special command once its argument has been accumulated. Color commands produced by
//! the LaTeX color package are converted into postscript; header commands are ignored, and others produce a warning.
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \return Zero on success, or a DVIE_* error code

int dviSpecialImplement(pplerr_context *ec, dviInterpreterState *interp) {
    char msg[PPL_MSG_LENGTH];
    int err = 0;
    char errString[SSTR_LENGTH];

    // Log the special command in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "DVI: dvi special. Final string=%s", interp->spString);
        ppl_log(ec, msg);
    }
    // Test for a colour string
    if (strncmp(interp->spString, "color ", 6) == 0) {
        if ((err = dviSpecialColourCommand(ec, interp, interp->spString + 6)) != 0) return err;
    }
    // Test for a header string
    else if (strncmp(interp->spString, "header=", 7) == 0) {
        // Header specials, which load postscript headers, are ignored
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "DVI: dvi special to load header ignored");
            ppl_log(ec, msg);
        }
    } else {
        // Unhandled special command (e.g. includegraphics)
        snprintf(errString, SSTR_LENGTH, "ignoring unhandled DVI special string %s", interp->spString);
        ppl_warning(ec, ERR_GENERIC, errString);
    }

    // Clean up
    //free(interp->spString);
    interp->spString = NULL;
    interp->special = 0;
    return err;
}

//! dviSpecialColourCommand - Implement a color special command, such as "color push rgb 1 0 0", by converting it into
//! postscript
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] command - The text of the special command, after the word "color"
//! \return Zero on success, or a DVIE_* error code

int dviSpecialColourCommand(pplerr_context *ec, const dviInterpreterState *interp, const char *command) {
    // Variables for the postscript command which sets the color
    char msg[PPL_MSG_LENGTH];
    char psText[SSTR_LENGTH];

    // Skip over any leading spaces
    while (command[0] == ' ') command++;

    // See what sort of colour command it is
    if (strncmp(command, "push ", 4) == 0) {
        // New colour to push onto stack
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "DVI: %s says push", command);
            ppl_log(ec, msg);
        }
        command += 5;
        while (command[0] == ' ') command++;
        // Push a CMYK, RGB, black or grey color onto the stack
        if (strncmp(command, "cmyk ", 5) == 0) {
            command += 5;
            snprintf(psText, SSTR_LENGTH, "%s setcmykcolor\n", command); // CMKY colour
        } else if (strncmp(command, "rgb ", 4) == 0) {
            command += 4;
            snprintf(psText, SSTR_LENGTH, "%s setrgbcolor\n", command); // rgb colour
        } else if (strncmp(command, "Black", 5) == 0) {
            snprintf(psText, SSTR_LENGTH, "0 0 0 setrgbcolor\n"); // black
        } else if (strncmp(command, "gray ", 5) == 0 || strncmp(command, "grey ", 5) == 0) // grey colour
        {
            command += 5;
            snprintf(psText, SSTR_LENGTH, "%s %s %s setrgbcolor\n", command, command, command);
        } else {
            snprintf(psText, SSTR_LENGTH, "failed to interpret dvi colour %s", command);
            ppl_warning(ec, ERR_INTERNAL, psText);
            return 0;
        }
        return dviSpecialColourStackPush(ec, interp, psText);
    } else if (strncmp(command, "pop", 3) == 0) {
        return dviSpecialColourStackPop(ec, interp); // Pop a colour off the stack
    } else {
        snprintf(psText, SSTR_LENGTH, "ignoring incomprehensible dvi colour command %s", command);
        ppl_warning(ec, ERR_INTERNAL, psText);
        return 0;
    }
    return 0;
}

//! dviSpecialColourStackPop - Pop a color from the color stack, and restore the color below it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \return Zero on success, or a DVIE_* error code

int dviSpecialColourStackPop(pplerr_context *ec, const dviInterpreterState *interp) {
    int err = 0;

    // Lop last colour off the end of the colour stack
    const char *item = (char *) ppl_listPop(interp->colStack);
    // Warn about popping from an empty color stack
    if (item == NULL) {
        ppl_warning(ec, ERR_INTERNAL, "dvi colour pop from empty stack");
        return 0;
    }

    // Read last colour from stack
    item = (char *) ppl_listLast(interp->colStack);

    // Set colour to item on top of stack
    if (item == NULL) {
        // Hit the bottom of the colour stack; returning to default colour. Spit out ASCII x01, and this will be dealt with later
        if ((err = dviPostscriptAppend(ec, interp, "\x01\n")) != 0) return err;
    } else {
        if ((err = dviPostscriptAppend(ec, interp, item)) != 0) return err;
    }
    return 0;
}

//! dviSpecialColourStackPush - Push a color onto the color stack, and switch to it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] psText - The postscript command which sets the color
//! \return Zero on success, or a DVIE_* error code

int dviSpecialColourStackPush(pplerr_context *ec, const dviInterpreterState *interp, const char *psText) {
    int err = 0;

    // Stick the string onto the stack
    const int len = strlen(psText) + 1;
    char *s = (char *) ppl_memAlloc(len);
    // Failure to allocate memory is reported as an error
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    snprintf(s, len, "%s", psText);
    ppl_listAppend(interp->colStack, (void *) s);

    // Also append to the postscript stack
    if ((err = dviPostscriptAppend(ec, interp, psText)) != 0) return err;
    return 0;
}

// ---------------------------------------
// Functions for manipulating interpreters
// ---------------------------------------

//! dviNewInterpreter - Create a new DVI interpreter, allocated in the current ppl_memAlloc context
//! \param [in] ec - The error-reporting context
//! \return The new interpreter, or NULL if memory allocation failed

dviInterpreterState *dviNewInterpreter(pplerr_context *ec) {
    // The interpreter, allocated in the current memory context
    dviInterpreterState *interp = (dviInterpreterState *) ppl_memAlloc(sizeof(dviInterpreterState));
    // Failure to allocate memory is reported as an error
    if (interp == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }

    // Initially the stack is empty
    interp->stack = ppl_listInit(0);
    // Failure to allocate memory is reported as an error
    if (interp->stack == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }
    interp->output = (postscriptState *) ppl_memAlloc(sizeof(postscriptState));
    // Failure to allocate memory is reported as an error
    if (interp->output == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }

    // No postscript yet
    interp->output->pages = ppl_listInit(0);
    // Failure to allocate memory is reported as an error
    if (interp->output->pages == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }
    interp->output->Npages = 0;
    interp->output->currentPage = NULL;
    interp->output->currentPosition = NULL;

    // Set default positional variables etc.
    interp->state = (dviStackState *) ppl_memAlloc(sizeof(dviStackState));
    // Failure to allocate memory is reported as an error
    if (interp->state == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }
    interp->state->h = 0;
    interp->state->v = 0;
    interp->state->w = 0;
    interp->state->x = 0;
    interp->state->y = 0;
    interp->state->z = 0;
    interp->f = 0;
    interp->curFnt = NULL;
    interp->boundingBox = NULL;
    interp->textSizeBox = NULL;
    interp->scale = 0.0;

    // No string currently being assembled
    interp->currentString = NULL;
    interp->currentStrlen = 0;

    // No fonts currently
    interp->fonts = ppl_listInit(0);
    // Failure to allocate memory is reported as an error
    if (interp->fonts == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }

    // Nothing special occuring
    interp->special = 0; // Not in special mode
    interp->spString = NULL;
    interp->colStack = ppl_listInit(0);
    // Failure to allocate memory is reported as an error
    if (interp->colStack == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }

    // Make the big table of operators
    makeDviOpTable(ec);
    return interp;
}

//! dviDeleteInterpreter - Release a DVI interpreter. Its memory is freed when its ppl_memAlloc context is freed.
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \return Zero

int dviDeleteInterpreter(pplerr_context *ec, dviInterpreterState *interp) {
    //free(interp->state);
    if (interp->currentStrlen != 0) {
        //free(interp->currentString);
        interp->currentStrlen = 0;
    }

    // Delete anything on the stack
    interp->stack = ppl_listInit(0);
    // Failure to allocate memory is reported as an error
    if (interp->stack == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }

    // Delete any remaining postscript output
    if (interp->output != NULL) dviDeletePostscriptState(ec, interp->output);

    // Delete the interpreter shell
    //free(interp);
    return 0;
}

//! dviCloneInterpState - Make a copy of a DVI stack state, which records the current position and spacing values
//! \param [in] ec - The error-reporting context
//! \param [in] orig - The DVI stack state to copy
//! \return The copy, or NULL if memory allocation failed

dviStackState *dviCloneInterpState(const pplerr_context *ec, const dviStackState *orig) {
    void *clone = ppl_memAlloc(sizeof(dviStackState));
    if (clone != NULL) memcpy(clone, (void *) orig, sizeof(dviStackState));
    return (dviStackState *) clone;
}

// ---------------------------------------------------------
// Functions for producing and manipulating postscript pages
// ---------------------------------------------------------

//! dviNewPostscriptPage - Create a new, empty postscript page, allocated in the current ppl_memAlloc context
//! \param [in] ec - The error-reporting context
//! \return The new page, or NULL if memory allocation failed

postscriptPage *dviNewPostscriptPage(pplerr_context *ec) {
    postscriptPage *page = (postscriptPage *) ppl_memAlloc(sizeof(postscriptPage));
    // Failure to allocate memory is reported as an error
    if (page == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }
    page->boundingBox = NULL;
    page->textSizeBox = NULL;
    //page->position[0] = 0;
    //page->position[1] = 0;
    page->text = ppl_listInit(0);
    // Failure to allocate memory is reported as an error
    if (page->text == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }
    //page->currentPosition = NULL; // No position set initially
    return page;
}

//! dviDeletePostscriptPage - Release a postscript page, including its list of postscript commands
//! \param [in] ec - The error-reporting context
//! \param [in] page - The postscript page
//! \return Zero

int dviDeletePostscriptPage(const pplerr_context *ec, postscriptPage *page) {
    // The bounding box is freed with the memory context
    if (page->boundingBox != NULL) {
        //free(page->boundingBox);
        page->boundingBox = NULL;
    }
    if (page->textSizeBox != NULL) { page->textSizeBox = NULL; }

    // free page->text
    //free(page);
    return 0;
}

//! dviDeletePostscriptState - Release a postscript state, including its list of pages
//! \param [in] ec - The error-reporting context
//! \param [in] state - The postscript state
//! \return Zero

int dviDeletePostscriptState(const pplerr_context *ec, postscriptState *state) {
    // free state->pages
    state->Npages = 0;
    state->currentPage = NULL;
    //free(state);
    return 0;
}

// -------------------------------------------
// Functions for producing postscript commands
// -------------------------------------------

//! dviPostscriptAppend - Append a postscript command to the current page
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] new - The postscript command to append
//! \return Zero on success, or a DVIE_* error code

int dviPostscriptAppend(pplerr_context *ec, const dviInterpreterState *interp, const char *new) {

    // Grab the new string and copy it into place
    const int len = strlen(new) + 1;
    char *s = (char *) ppl_memAlloc(len); // A temporary string pointer
    // Failure to allocate memory is reported as an error
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    strncpy(s, new, len);
    ppl_listAppend(interp->output->currentPage->text, (void *) s);
    return 0;
}

//! dviPostscriptMoveto - Append a postscript moveto command to the current page, to move to the current DVI position,
//! if the postscript position does not already match it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \return Zero on success, or a DVIE_* error code

int dviPostscriptMoveto(pplerr_context *ec, dviInterpreterState *interp) {
    int err = 0;
    char s[SSTR_LENGTH];
    const double x = interp->state->h * interp->scale;
    const double y = 765 - interp->state->v * interp->scale;
    snprintf(s, SSTR_LENGTH, "newpath\n%f %f moveto\n", x, y);
    if ((err = dviPostscriptAppend(ec, interp, s)) != 0) return err;

    // If we don't have a current position make one, else set the current ps position to the dvi one
    if (interp->output->currentPosition == NULL) {
        interp->output->currentPosition = dviCloneInterpState(ec, interp->state);
        if (interp->output->currentPosition == NULL) return DVIE_MEMORY;
    } else {
        interp->output->currentPosition->h = interp->state->h;
        interp->output->currentPosition->v = interp->state->v;
    }
    return 0;
}

//! dviPostscriptLineto - Append a postscript lineto command to the current page, to draw a line to the current DVI
//! position
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \return Zero on success, or a DVIE_* error code

int dviPostscriptLineto(pplerr_context *ec, const dviInterpreterState *interp) {
    int err = 0;
    char s[SSTR_LENGTH];
    const double x = interp->state->h * interp->scale;
    const double y = 765 - interp->state->v * interp->scale;
    snprintf(s, SSTR_LENGTH, "%f %f lineto\n", x, y);
    if ((err = dviPostscriptAppend(ec, interp, s)) != 0) return err;

    // If we don't have a current position make one, else set the current ps position to the dvi one
    if (interp->output->currentPosition == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "postscript error: lineto command issued with NULL current state!");
        return DVIE_INTERNAL;
    } else {
        interp->output->currentPosition->h = interp->state->h;
        interp->output->currentPosition->v = interp->state->v;
    }
    return 0;
}

//! dviPostscriptClosepathFill - Append postscript commands to close and fill the current path
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \return Zero on success, or a DVIE_* error code

int dviPostscriptClosepathFill(pplerr_context *ec, const dviInterpreterState *interp) {
    int err = 0;
    char s[SSTR_LENGTH];
    //double x, y;
    //x = interp->state->h * interp->scale;
    //y = 765 - interp->state->v * interp->scale;
    snprintf(s, SSTR_LENGTH, "closepath fill\n");
    if ((err = dviPostscriptAppend(ec, interp, s)) != 0) return err;
    // Report a closepath with no current point
    if (interp->output->currentPosition == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "postscript error: closepath command issued with NULL current state!");
        return DVIE_INTERNAL;
    } else {
        //free(interp->output->currentPosition);
        interp->output->currentPosition = NULL;
    }
    return 0;
}

//! dviTypeset - Typeset the run of characters accumulated in the interpreter's current string, writing postscript to
//! show them and updating the bounding box of the page
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \return Zero on success, or a DVIE_* error code

int dviTypeset(pplerr_context *ec, dviInterpreterState *interp) {
    // Variables for typesetting the run of characters
    char msg[PPL_MSG_LENGTH];
    // This subroutine does the bulk of the actual postscript work, typesetting runs of characters
    dviStackState *postPos, *dviPos; // Current positions in dvi and postscript code
    double size[4]; // Width, height, depth
    int err = 0;

    // Move to the current position before typesetting, if the postscript position differs from it
    postPos = interp->output->currentPosition;
    dviPos = interp->state;

    // First check if we need to move before typesetting
    if (postPos == NULL) {
        if ((err = dviPostscriptMoveto(ec, interp)) != 0) return err;
        interp->output->currentPosition = dviCloneInterpState(ec, dviPos);
        if (interp->output->currentPosition == NULL) return DVIE_MEMORY;
    } else if (postPos->h != dviPos->h || postPos->v != dviPos->v) {
        if ((err = dviPostscriptMoveto(ec, interp)) != 0) return err;
    }

    // Measure the run of characters: its total width, and its greatest height and depth
    char *s = interp->currentString;
    double width = 0.0;
    double height = 0.0;
    double depth = 0.0;
    double italic = 0.0;
    // Add the width of each character
    while (*s != '\0') {
        if ((err = dviGetCharSize(ec, interp, (unsigned char) *s, size)) != 0) return err;
        width += size[0];
        height = size[1] > height ? size[1] : height;
        depth = size[2] > depth ? size[2] : depth;
        italic = size[3];
        s++;
    }

    // Convert back into dvi units
    width /= interp->scale;
    height /= interp->scale;
    depth /= interp->scale;
    italic /= interp->scale;
    // Log the size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "DVI: width of glyph %g height of glyph %g", width, height);
        ppl_log(ec, msg);
    }

    // Only need to consider extra italic width for the final glyph
    if ((err = dviUpdateBoundingBox(ec, interp, width + italic, height, depth)) != 0) return err;

    // Count the number of characters to write to the ps string
    const int chars = strlen(interp->currentString) + 9;
    s = (char *) ppl_memAlloc(chars);
    // Failure to allocate memory is reported as an error
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    snprintf(s, chars, "(%s) show\n", interp->currentString);

    // Send the string off to the postscript routine and clean up memory
    if ((err = dviPostscriptAppend(ec, interp, s)) != 0) return err;
    //free(s);
    //free(interp->currentString);
    interp->currentString = NULL;
    interp->currentStrlen = 0;

    // Adjust the current position
    interp->state->h += width;
    interp->output->currentPosition->h += width;
    return 0;
}

//! dviChngFnt - Change the current font, writing postscript to select it
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] fn - The number of the font to select, as defined by a DVI_FNTDEF operator
//! \return Zero on success, or a DVIE_* error code

int dviChngFnt(pplerr_context *ec, dviInterpreterState *interp, const int fn) {
    // Variables for selecting the font
    char msg[PPL_MSG_LENGTH];
    int err = 0;

    // Record the font number
    interp->f = fn;

    // Find the font in the list
    interp->curFnt = NULL;
    listIterator *listIter = ppl_listIterateInit(interp->fonts);
    // Find the font with this number among those defined
    while (listIter != NULL) {
        // This is the font
        if (((dviFontDetails *) listIter->data)->number == fn) {
            interp->curFnt = (dviFontDetails *) listIter->data;
            break;
        }
        ppl_listIterate(&listIter);
    }

    // See whether we found the font, and if not, throw an error
    if (interp->curFnt == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Corrupt DVI file: failed to find current font");
        return DVIE_CORRUPT;
    }

    // Produce an appropriate postscript command
    dviFontDetails *font = (dviFontDetails *) listIter->data;
    const int len = strlen(font->psName) + 20;
    char *s = (char *) ppl_memAlloc(len);
    // Failure to allocate memory is reported as an error
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    const double size = font->useSize * interp->scale;
    // Log the font size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "DVI: Font useSize %d size %g changed to %d", font->useSize, size,
                          (int) ceil(size - .5));
        ppl_log(ec, msg);
    }
    snprintf(s, len, "/%s %d selectfont\n", font->psName, (int) ceil(size - .5));
    if ((err = dviPostscriptAppend(ec, interp, s)) != 0) return err;
    //free(s);
    return 0;
}

//! dviGetCharSize - Look up the dimensions of a character in the current font
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] s - The character code
//! \param [out] size - Array into which to write the width, height, depth and italic correction of the character, in
//! DVI units
//! \return Zero on success, or a DVIE_* error code

int dviGetCharSize(pplerr_context *ec, const dviInterpreterState *interp, unsigned char s, double *size) {
    // Variables for looking up the character in the font metrics
    char msg[PPL_MSG_LENGTH];
    int wi, hi, di, ii; // Indices

    // Find the character's entry in the font's metrics
    const dviFontDetails *font = (dviFontDetails *) interp->curFnt; // Font information (for tfm and use size)
    const dviTFM *tfm = font->tfm; // Details of this font
    const int chnum = s - tfm->bc; // Character number in this font
    const TFMcharInfo *chin = tfm->charInfo + chnum; // Character info for this character
    const double scale = font->useSize * interp->scale; // Font use size * unit scaling

    // Look up its width, height, depth and italic correction, scaled to the font's size in postscript points
    wi = (int) chin->wi;
    hi = (int) chin->hi;
    di = (int) chin->di;
    ii = (int) chin->ii;
    size[0] = tfm->width[wi] * scale;
    size[1] = tfm->height[hi] * scale;
    size[2] = tfm->depth[di] * scale;
    size[3] = tfm->italic[ii] * scale;

    // The following loop adds a small quantity of padding as the supplied sizes appear to be somewhat insufficient
    size[1] += 0.05 * scale;
    size[2] += 0.05 * scale;

    // Log the size in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "DVI: Character %d chnum %d has indices %d %d %d %d width %g height %g depth %g italic %g useSize %g desSize %g",
                s, chnum, wi, di, hi, ii, size[0], size[1], size[2], size[3], font->useSize * interp->scale,
                font->desSize * interp->scale);
        ppl_log(ec, msg);
    }
    return 0;
}

//! dviUpdateBoundingBox - Expand the bounding box of the current page, and of the current text item, to include an
//! object at the current position
//! \param [in] ec - The error-reporting context
//! \param [in] interp - The state of the DVI interpreter
//! \param [in] width - The width of the object, in DVI units
//! \param [in] height - The height of the object above the baseline, in DVI units
//! \param [in] depth - The depth of the object below the baseline, in DVI units
//! \return Zero on success, or a DVIE_* error code

int dviUpdateBoundingBox(pplerr_context *ec, dviInterpreterState *interp, const double width, const double height,
                         const double depth) {
    double *bb;
    double bbObj[4]; // Bounding box of the object that we are typeseting

    // left bottom right top
    // DVI counts down from the top (and this is all DVI units)
    bbObj[0] = interp->state->h;
    bbObj[1] = interp->state->v + depth;
    bbObj[2] = interp->state->h + width;
    bbObj[3] = interp->state->v - height;

    // Check to see if we already have a bounding box
    if (interp->boundingBox == NULL) {
        bb = (double *) ppl_memAlloc(4 * sizeof(double));
        // Failure to allocate memory is reported as an error
        if (bb == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return DVIE_MEMORY;
        }
        bb[0] = bbObj[0];
        bb[1] = bbObj[1];
        bb[2] = bbObj[2];
        bb[3] = bbObj[3];
        interp->boundingBox = bb;
    } else {
        // Check against current bounding box
        bb = interp->boundingBox;
        bb[0] = bb[0] < bbObj[0] ? bb[0] : bbObj[0];
        bb[1] = bb[1] > bbObj[1] ? bb[1] : bbObj[1];
        bb[2] = bb[2] > bbObj[2] ? bb[2] : bbObj[2];
        bb[3] = bb[3] < bbObj[3] ? bb[3] : bbObj[3];
    }

    // Now repeat the process for the text size box
    // This is the baseline and 70% of the use height, which we take to be the cap height
    bbObj[1] = interp->state->v;
    bbObj[3] = interp->state->v - interp->curFnt->useSize * 0.70;

    // Check to see if we already have a text size box
    if (interp->textSizeBox == NULL) {
        bb = (double *) ppl_memAlloc(4 * sizeof(double));
        // Failure to allocate memory is reported as an error
        if (bb == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return DVIE_MEMORY;
        }
        bb[0] = bbObj[0];
        bb[1] = bbObj[1];
        bb[2] = bbObj[2];
        bb[3] = bbObj[3];
        interp->textSizeBox = bb;
    } else {
        // Check against current bounding box
        bb = interp->textSizeBox;
        bb[0] = bb[0] < bbObj[0] ? bb[0] : bbObj[0];
        bb[1] = bb[1] > bbObj[1] ? bb[1] : bbObj[1];
        bb[2] = bb[2] > bbObj[2] ? bb[2] : bbObj[2];
        bb[3] = bb[3] < bbObj[3] ? bb[3] : bbObj[3];
    }

    // Done
    return 0;
}
