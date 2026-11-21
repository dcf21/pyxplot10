// dvi_read.c
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

// Reads the dvi files which LaTeX produces when it typesets the text of a canvas. Each operator in the file is read,
// with its arguments, and passed to the interpreter in dvi_interpreter.c, which turns each page of text into
// postscript; canvasDraw.c then places these pages wherever text appears on the canvas. Reading stops at the postamble.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "epsMaker/dvi_read.h"
#include "epsMaker/dvi_font.h"

// The following is useful for output sometimes
const char *dviops[58] = {"set1",    "set2",    "set3",    "set4",    "setrule",  "put1",     "put2",     "put3",
                          "put4",    "putrule", "nop",     "bop",     "eop",      "push",     "pop",      "right1",
                          "right2",  "right3",  "right4",  "w0",      "w1",       "w2",       "w3",       "w4",
                          "x0",      "x1",      "x2",      "x3",      "x4",       "down1",    "down2",    "down3",
                          "down4",   "y0",      "y1",      "y2",      "y3",       "y4",       "z0",       "z1",
                          "z2",      "z3",      "z4", // Big hole in the labels here...
                          "fnt1",    "fnt2",    "fnt3",    "fnt4",    "special1", "special2", "special3", "special4",
                          "fntdef1", "fntdef2", "fntdef3", "fntdef4", "pre",      "post",     "postpost"};

//! ReadDviFile - Read a DVI file produced by LaTeX, and interpret it to produce postscript for each page
//! \param [in] ec - The error-reporting context
//! \param [in] filename - The path of the DVI file
//! \param [out] status - Set to zero on success, or a DVIE_* error code
//! \return The DVI interpreter, containing the postscript for each page, or NULL on failure

dviInterpreterState *ReadDviFile(pplerr_context *ec, const char *filename, int *status) {
    // Variables for reading the file
    DVIOperator op;
    int i;
    int POST = 0;
    int err;

    // The operator has no string arguments yet
    op.s[0] = NULL;
    op.s[1] = NULL;

    // Set up the interpreter
    dviInterpreterState *interpreter = dviNewInterpreter(ec);

    // Open the dvi file
    FILE *fp = fopen(filename, "r");
    // Report a failure to open the file
    if (fp == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "dvi file does not exist!");
        *status = 1;
        return NULL;
    }

    // This is the main loop for the dvi parser
    while (!feof(fp)) {
        // Read the next operator from the dvi file
        if ((err = GetDVIOperator(ec, &op, fp)) != 0) {
            // Give up on any error
            if (err) {
                *status = err;
                fclose(fp);
                return NULL;
            } else {
                continue;
            }
        }

        if (DEBUG) DisplayDVIOperator(ec, &op); // Producing debugging output

        // A slightly more sophisticated interpreter that makes some postscript
        if (!POST) {
            err = dviInterpretOperator(ec, interpreter, &op);
            // Give up on any error
            if (err) {
                *status = err;
                fclose(fp);
                return NULL;
            }
        }
        // Forget the operator's string arguments, which are freed with the memory context
        for (i = 0; i < 2; i++) {
            // Each string argument
            if (op.s[i] != NULL) {
                //free(op.s[i]);
                op.s[i] = NULL;
            }
        }

        // If we've hit the post then we can break out
        if (op.op == DVI_POST) {
            POST = 1;
            break;
        }
    }

    // Return the interpreter, which holds the postscript for each page
    fclose(fp);
    return interpreter;
}

//! GetDVIOperator - Read a DVI operator from a file, together with any additional data which it takes. A reference for
//! the meaning and size of the data is http://www-users.math.umd.edu/~asnowden/comp-cont/dvi.h
//! \param [in] ec - The error-reporting context
//! \param [out] op - The operator
//! \param [in] fp - The file handle of the DVI file
//! \return Zero on success, or a DVIE_* error code

int GetDVIOperator(pplerr_context *ec, DVIOperator *op, FILE *fp) {
    // Variables for reading the operator
    int i, v, err;

    // First read in the opcode
    if ((err = ReadUChar(ec, fp, &v)) != 0) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Error reading dvi operator from disk");
        return err;
    }
    op->op = v;

    // Now work out what it represents and get any extra data if needed
    if (v < DVI_CHARMIN) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Illegal opcode whilst parsing DVI file");
        return 1;
    } else if (v >= DVI_CHARMIN && v <= DVI_CHARMAX) {
        return 0;
    } else if (v < DVI_SET1234 + 4) {
        int Ndata = v - DVI_SET1234 + 1;
        if ((err = ReadLongInt(ec, fp, op->ul, Ndata)) != 0) return err;

        // Set to DVI_SET1234 for convenience later (we don't need to know whether it's 1, 2, 3 or 4)
        // op->op = DVI_SET1234;
        return 0;
    } else if (v == DVI_SETRULE) {
        if ((err = ReadSignedInt(ec, fp, op->sl, 4)) != 0) return err;
        if ((err = ReadSignedInt(ec, fp, op->sl + 1, 4)) != 0) return err;
        return 0;
    } else if (v < DVI_PUT1234 + 4) {
        int Ndata = v - DVI_PUT1234 + 1;
        if ((err = ReadLongInt(ec, fp, op->ul, Ndata)) != 0) return err;
        return 0;
    } else if (v == DVI_PUTRULE) {
        if ((err = ReadSignedInt(ec, fp, op->sl, 4)) != 0) return err;
        if ((err = ReadSignedInt(ec, fp, op->sl + 1, 4)) != 0) return err;
        return 0;
    } else if (v == DVI_NOP) {
        return 0;
    } else if (v == DVI_BOP) {
        for (i = 0; i < 10; i++) {
            if ((err = ReadLongInt(ec, fp, op->ul + i, 4)) != 0) return err;
        }
        if ((err = ReadSignedInt(ec, fp, op->sl, 4)) != 0) return err;
        return 0;
    } else if (v >= DVI_EOP && v <= DVI_POP) {
        return 0;
    } else if (v < DVI_RIGHT1234 + 4) {
        int Ndata = v - DVI_RIGHT1234 + 1;
        if ((err = ReadSignedInt(ec, fp, op->sl, Ndata)) != 0) return err;

        // Set to DVI_RIGHT1234 for convenience later (we don't need to know whether it's 1, 2, 3 or 4)
        // op->op = DVI_RIGHT1234;
        return 0;
    } else if (v == DVI_W0) {
        return 0;
    } else if (v < DVI_W1234 + 4) {
        int Ndata = v - DVI_W1234 + 1;
        if ((err = ReadSignedInt(ec, fp, op->sl, Ndata)) != 0) return err;
        return 0;
    } else if (v == DVI_X0) {
        return 0;
    } else if (v < DVI_X1234 + 4) {
        int Ndata = v - DVI_X1234 + 1;
        if ((err = ReadSignedInt(ec, fp, op->sl, Ndata)) != 0) return err;
        return 0;
    } else if (v < DVI_DOWN1234 + 4) {
        int Ndata = v - DVI_DOWN1234 + 1;
        if ((err = ReadSignedInt(ec, fp, op->sl, Ndata)) != 0) return err;
        return 0;
    } else if (v == DVI_Y0) {
        return 0;
    } else if (v < DVI_Y1234 + 4) {
        int Ndata = v - DVI_Y1234 + 1;
        if ((err = ReadSignedInt(ec, fp, op->sl, Ndata)) != 0) return err;
        return 0;
    } else if (v == DVI_Z0) {
        return 0;
    } else if (v < DVI_Z1234 + 4) {
        int Ndata = v - DVI_Z1234 + 1;
        if ((err = ReadSignedInt(ec, fp, op->sl, Ndata)) != 0) return err;
        return 0;
    } else if (v >= DVI_FNTNUMMIN && v <= DVI_FNTNUMMAX) {
        return 0;
    } else if (v <= DVI_FNT1234) {
        int Ndata = v - DVI_FNT1234 + 1;
        if ((err = ReadLongInt(ec, fp, op->ul, Ndata)) != 0) return err;
        return 0;

    } else if (v <= DVI_SPECIAL1234) {
        int Ndata = v - DVI_SPECIAL1234 + 1;
        if ((err = ReadLongInt(ec, fp, op->ul, Ndata)) != 0) return err;
        return 0;
    } else if (v <= DVI_FNTDEF1234) {
        int i;
        int Ndata = v - DVI_FNTDEF1234 + 1;
        if ((err = ReadLongInt(ec, fp, op->ul, Ndata)) != 0) return err;
        for (i = 1; i < 4; i++) {
            if ((err = ReadLongInt(ec, fp, op->ul + i, 4)) != 0) return err;
        }
        for (i = 4; i < 6; i++) {
            if ((err = ReadLongInt(ec, fp, op->ul + i, 1)) != 0) return err;
        }
        Ndata = op->ul[4] + op->ul[5];
        op->s[0] = (char *) ppl_memAlloc(Ndata + 1);
        // Failure to allocate memory is reported as an error
        if (op->s[0] == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return DVIE_MEMORY;
        }
        op->s[0][Ndata] = '\0';
        // Read the string, byte by byte
        for (i = 0; i < Ndata; i++) {
            int j;
            if ((err = ReadUChar(ec, fp, &j)) != 0) return err;
            op->s[0][i] = (char) j;
        }
        return 0;
    } else if (v == DVI_PRE) {
        int i, v1;
        if ((err = ReadUChar(ec, fp, &v1)) != 0) return err;
        op->ul[0] = v1;
        for (i = 1; i < 4; i++) {
            if ((err = ReadLongInt(ec, fp, op->ul + i, 4)) != 0) return err;
        }
        if ((err = ReadUChar(ec, fp, &v1)) != 0) return err;

        // Throw away the DVI comment
        for (i = v1; i > 0; i--) {
            if ((err = ReadUChar(ec, fp, &v1)) != 0) return err;
        }
        return 0;
    } else if (v == DVI_POST) {
        for (i = 0; i < 6; i++) {
            if ((err = ReadLongInt(ec, fp, op->ul + i, 4)) != 0) return err;
        }
        for (i = 6; i < 8; i++) {
            if ((err = ReadLongInt(ec, fp, op->ul + i, 2)) != 0) return err;
        }
        return 0;
    } else if (v == DVI_POSTPOST) {
        int v1;
        if ((err = ReadLongInt(ec, fp, op->ul, 4)) != 0) return err;
        if ((err = ReadUChar(ec, fp, &v1)) != 0) return err;
        op->ul[1] = v1;
        // Mop up all the 223s
        //  while (!feof(fp))
        //   {
        //    if ((err=ReadUChar(ec, fp,&v1))!=0) return err;
        //    if (v1 != 223) return v1;
        //   }
        return 0;
    } else {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Unidentified opcode in dvi file");
        return 1;
    }

    // Once we've got an operator we return, so we should never get here
    ppl_error(ec, ERR_INTERNAL, -1, -1, "GetDVIOperator: flow control has gone wrong");
    return 1;
}

//! ReadUChar - Read an unsigned char from a DVI file
//! \param [in] ec - The error-reporting context
//! \param [in] fp - The file handle of the DVI file
//! \param [out] uc - Set to the value read
//! \return Zero on success, or a DVIE_* error code

int ReadUChar(pplerr_context *ec, FILE *fp, int *uc) {
    int i = getc(fp);
    // Report an unexpected end of file
    if (i == EOF) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Unexpected EOF in dvi file");
        return 1;
    }
    *uc = i;
    return 0;
}

//! ReadLongInt - Read an unsigned big-endian integer of 1-4 bytes from a DVI file
//! \param [in] ec - The error-reporting context
//! \param [in] fp - The file handle of the DVI file
//! \param [out] uli - Set to the value read
//! \param [in] n - The number of bytes to read
//! \return Zero on success, or a DVIE_* error code

int ReadLongInt(pplerr_context *ec, FILE *fp, unsigned long int *uli, const int n) {
    int err, v, i;
    if ((err = ReadUChar(ec, fp, &v)) != 0) return err;
    unsigned long int fv = v;
    // Read each further byte, most significant first
    for (i = 0; i < n - 1; i++) {
        if ((err = ReadUChar(ec, fp, &v)) != 0) return err;
        fv = (fv << 8) | v;
    }
    *uli = fv;
    return 0;
}

//! ReadSignedInt - Read a signed big-endian integer of 1-4 bytes from a DVI file
//! \param [in] ec - The error-reporting context
//! \param [in] fp - The file handle of the DVI file
//! \param [out] sli - Set to the value read
//! \param [in] n - The number of bytes to read
//! \return Zero on success, or a DVIE_* error code

int ReadSignedInt(pplerr_context *ec, FILE *fp, signed long int *sli, const int n) {
    int err, v, i;
    if ((err = ReadUChar(ec, fp, &v)) != 0) return err;
    signed long int fv = v < 128 ? v : v - 256;
    // Read each further byte, most significant first
    for (i = 0; i < n - 1; i++) {
        if ((err = ReadUChar(ec, fp, &v)) != 0) return err;
        fv = (fv * 256) + v; // Not (fv << 8), since left-shifting a negative value is undefined behaviour
    }
    *sli = fv;
    return 0;
}

//! DisplayDVIOperator - Write a log message describing a DVI operator, for debugging
//! \param [in] ec - The error-reporting context
//! \param [in] op - The operator
//! \return Zero

int DisplayDVIOperator(pplerr_context *ec, DVIOperator *op) {
    // The message to log
    char s2[128];
    char *s = NULL;
    // Describe the operator, with its arguments, according to its type
    if (op->op <= DVI_CHARMAX) {
        int i = op->op;
        if ((i >= 31) && (i <= 126)) snprintf(s2, 128, "DVI: Read character %d %c", i, i);
        else snprintf(s2, 128, "DVI: Read character %d", i);
        s = s2;
    } else if (op->op == DVI_PRE) // Special cases
    {
        snprintf(s2, 128, "DVI: Read %s %lu %lu %lu %lu", "pre", *(op->ul), *(op->ul + 1), *(op->ul + 2),
                 *(op->ul + 3));
        s = s2;
    } else if (op->op == DVI_FNTDEF1234) {
        snprintf(s2, 128, "DVI: Read %s N=%lu d=%lu n=%s", "fnt def", *(op->ul), *(op->ul + 4), (op->s[0]));
        s = s2;
    } else if (op->op < DVI_FNTNUMMIN) {
        s = (char *) dviops[op->op - DVI_CHARMAX - 1];
        // Operators whose names end in a digit take a numeric argument
        if (strlen(s) == 2 && (char) s[1] > '0') {
            snprintf(s2, 128, "DVI: Read %s %lu", dviops[op->op - DVI_CHARMAX - 1], *(op->ul));
            s = s2;
        } else {
            snprintf(s2, 128, "DVI: Read %s", s);
            s = s2;
        }
    } else if (op->op <= DVI_FNTNUMMAX) {
        snprintf(s2, 128, "DVI: Read font number %d", op->op - DVI_FNTNUMMIN);
        s = s2;
    } else if (op->op <= DVI_POSTPOST) {
        int i = op->op - DVI_CHARMAX - 1 - (DVI_FNTNUMMAX - DVI_FNTNUMMIN + 1);
        snprintf(s2, 128, "DVI: Read %s", (char *) dviops[i]);
        s = s2;
        //s = (char *)dviops[i];
    } else {
        snprintf(s2, 128, "DVI: dvi error");
        s = s2;
    }
    ppl_log(ec, s);
    return 0;
}
