// dvi_font.c
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

// Functions for manupulating dvi files -- font section

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "coreUtils/errorReport.h"
#include "stringTools/asciidouble.h"
#include "coreUtils/memAlloc.h"
#include "stringTools/strConstants.h"
#include "epsMaker/dvi_read.h"
#include "epsMaker/dvi_font.h"
#include "epsMaker/kpse_wrap.h"

#define N_BUILTIN_FONTS   5
#define L_BUILTIN_FNAME   10
#define L_BUILTIN_FPSNAME 23

char builtinFonts[N_BUILTIN_FONTS][L_BUILTIN_FNAME] = {"ptmb7t", "ptmr7t", "ptmri7t", "phvr7t", "pcrr7t"};
char builtinFontNames[N_BUILTIN_FONTS][L_BUILTIN_FPSNAME] = {"Times-Bold", "Times-Roman", "Times-Italic", "Helvetica",
                                                             "Courier"};


int dviGetPfa(pplerr_context *ec, dviFontDetails *font, char *filename);
static int dviGetFontFromMap(pplerr_context *ec, dviFontDetails *font);

//! dviGetTFM - Locate and read the TeX font metric (TFM) file for a font
//! \param [in] ec - The error-reporting context
//! \param [in] font - The font, whose metrics are loaded into it
//! \return Zero on success, or a DVIE_* error code

int dviGetTFM(pplerr_context *ec, dviFontDetails *font) {
    char msg[PPL_MSG_LENGTH];
    char *TFMpath;
    char *s;
    char errStr[SSTR_LENGTH];
    int err = 0;
    int i;
    FILE *TFMfp;

    // Get the TFM file
    s = (char *) ppl_memAlloc(strlen(font->name) + 5);
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    sprintf(s, "%s.tfm", font->name);
    TFMpath = ppl_kpse_wrap_find_tfm(ec, s);
    if (TFMpath == NULL) {
        snprintf(errStr, SSTR_LENGTH,
                 "Could not find TFM file '%s'. You may be able to fix this with a more complete installation of tex.",
                 s);
        ppl_error(ec, ERR_INTERNAL, -1, -1, errStr);
        return 1;
    }
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Font file %s: TFM path: %s", font->name, TFMpath);
        ppl_log(ec, msg);
    }
    TFMfp = fopen(TFMpath, "r");
    if (TFMfp == NULL) {
        snprintf(errStr, SSTR_LENGTH,
                 "Could not open TFM file '%s'. You may be able to fix this with a more complete installation of tex.",
                 s);
        ppl_error(ec, ERR_INTERNAL, -1, -1, errStr);
        return 1;
    }
    font->tfm = dviReadTFM(ec, TFMfp, &err);
    fclose(TFMfp);
    if (err) return err;

    // Find the maximum height and depth of the font
    err = dviFindMaxSize(ec, font);
    if (err) return err;

    // Work out what type of font this is
    if (strncmp(font->tfm->coding, "TeX text", 8) == 0) font->fontType = FONT_TEX_TEXT;
    else if (strncmp(font->tfm->coding, "TeX math italic", 15) == 0) font->fontType = FONT_TEX_MATH;
    else if (strncmp(font->tfm->coding, "TeX math extension", 18) == 0) font->fontType = FONT_TEX_MEXT;
    else if (strncmp(font->tfm->coding, "TeX math symbols", 16) == 0) font->fontType = FONT_TEX_MSYM;
    else font->fontType = FONT_UNKNOWN;

    // This is a list of bonkers fonts
    if (strncmp(font->tfm->family, "WASY", 4) == 0) font->fontType = FONT_SYMBOL;

    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: font type %d", font->fontType);
        ppl_log(ec, msg);
    }

    // Deal with built-in fonts
    font->psName = NULL;
    for (i = 0; i < N_BUILTIN_FONTS; i++) {
        if (strlen(font->name) == strlen(builtinFonts[i]) &&
            strncmp(font->name, builtinFonts[i], L_BUILTIN_FNAME) == 0) {
            font->pfaPath = NULL;
            font->psName = (char *) ppl_memAlloc(strlen(builtinFontNames[i]) + 1);
            if (font->name == NULL) {
                ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
                return DVIE_MEMORY;
            }
            strcpy(font->psName, builtinFontNames[i]);
            if (DEBUG) {
                snprintf(errStr, SSTR_LENGTH, "Found builtin font %sXXX%sXXX%sXXX %d", builtinFonts[i], font->psName,
                         builtinFontNames[i], i);
                ppl_log(ec, errStr);
            }
            break;
        }
    }

    /*
  if (strlen(font->name)==6 && strncmp(font->name, "ptmb7t", 6)==0)
   {
    // Times-Bold
    font->pfaPath = NULL;
    font->psName = (char *)ppl_memAlloc(11);
    if (font->name==NULL) { ppl_error(ec, ERR_MEMORY,"Out of memory"); return DVIE_MEMORY; }
    snprintf(font->psName, 11, "Times-Bold");
   }
  else if (strlen(font->name)==6 && strncmp(font->name, "ptmr7t", 6)==0)
   {
    // Times-Roman
    font->pfaPath = NULL;
    font->psName = (char *)ppl_memAlloc(12);
    if (font->name==NULL) { ppl_error(ec, ERR_MEMORY,"Out of memory"); return DVIE_MEMORY; }
    snprintf(font->psName, 12, "Times-Roman");
   }
  else */
    if (font->psName == NULL) // Not found a built-in font
    {
        // Obtain the pfa file. First look for a type 1 font with the same name as the TeX font (e.g. cmr10.pfb).
        // Failing that, look the font up in the TeX font map psfonts.map, which may say that it is a re-encoded
        // version of another font (e.g. tcrm1000 is sfrm1000.pfb with the TS1 encoding). Finally, try the family
        // name given in the TFM file.
        err = dviGetPfa(ec, font, font->name);
        if (err == DVIE_NOFONT) err = dviGetFontFromMap(ec, font);
        if (err == DVIE_NOFONT) err = dviGetPfa(ec, font, font->tfm->family);
        if (err == DVIE_NOFONT) {
            snprintf(errStr, SSTR_LENGTH, "dviGetTfm: Cannot find pfa or pfb file for font %s", font->name);
            ppl_error(ec, ERR_GENERIC, -1, -1, errStr);
        }
        if (err != 0) return err;

        // Obtain the font name from the PFA file, unless the font map has already supplied it
        if (font->psName == NULL) {
            font->psName = psNameFromPFA(ec, font->pfaPath);
            if (font->psName == NULL) return 1;
        }
    }
    if (font->basePsName == NULL) font->basePsName = font->psName;

    return 0;
}


//! dviGetPfa - Locate the type 1 font file for a font, in PFA or PFB format, and convert it to PFA format if necessary,
//! for embedding in postscript output
//! \param [in] ec - The error-reporting context
//! \param [in] font - The font, whose postscript name is set
//! \param [out] filename - Character buffer into which to write the path of the PFA file
//! \return Zero on success, or a DVIE_* error code

int dviGetPfa(pplerr_context *ec, dviFontDetails *font, char *filename) {
    FILE *fpin, *fpout;
    char errStr[SSTR_LENGTH];
    char *s, *PFApath, *PFBpath;
    int err, i;

    s = (char *) ppl_memAlloc(strlen(filename) + 5);
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    sprintf(s, "%s.pfa", filename);
    // Crude lower-casing
    for (i = 0; i < strlen(filename); i++)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = s[i] + 'a' - 'A';
    PFApath = ppl_kpse_wrap_find_pfa(ec, s);
    if (PFApath != NULL) {
        font->pfaPath = PFApath;
    } else {
        // Make a PFA file from the PFB file (assuming that one exists)
        sprintf(s, "%s.pfb", filename);
        // Crude lower-casing
        for (i = 0; i < strlen(filename); i++)
            if (s[i] >= 'A' && s[i] <= 'Z') s[i] = s[i] + 'a' - 'A';
        PFBpath = ppl_kpse_wrap_find_pfb(ec, s);
        if (PFBpath == NULL) {
            snprintf(errStr, SSTR_LENGTH, "dviGetTfm: Cannot find pfa or pfb file for font %s using name %s",
                     font->name, filename);
            ppl_log(ec, errStr);
            return DVIE_NOFONT;
        }
        fpin = fopen(PFBpath, "r");
        if (fpin == NULL) {
            snprintf(errStr, SSTR_LENGTH, "dviGetTfm: Cannot open pfb file %s", PFBpath);
            ppl_error(ec, ERR_GENERIC, -1, -1, errStr);
            return DVIE_ACCESS;
        }

        // Make a filename for the destination pfa file
        // free(PFApath);
        PFApath = (char *) ppl_memAlloc(SSTR_LENGTH);
        if (PFApath == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            fclose(fpin);
            return DVIE_MEMORY;
        }
        if (snprintf(PFApath, SSTR_LENGTH, "%s%s%s.pfa", ec->session_default.tempdir, PATHLINK, font->name) >=
            SSTR_LENGTH) {
            ppl_error(ec, ERR_INTERNAL, -1, -1, "Path of temporary PFA font file is too long");
            fclose(fpin);
            return DVIE_INTERNAL;
        }
        fpout = fopen(PFApath, "w");
        if (fpout == NULL) {
            snprintf(errStr, SSTR_LENGTH, "dviGetTfm: Cannot write to pfa file %s", PFApath);
            ppl_error(ec, ERR_GENERIC, -1, -1, errStr);
            fclose(fpin);
            return DVIE_ACCESS;
        }
        if ((err = pfb2pfa(ec, fpin, fpout)) != 0) {
            if (DEBUG) {
                snprintf(errStr, SSTR_LENGTH, "Fail in pfb2pfa: %d", err);
                ppl_log(ec, errStr);
            }
            fclose(fpin);
            fclose(fpout);
            return err;
        }
        fclose(fpin);
        fclose(fpout);
        font->pfaPath = PFApath;
    }
    return 0;
}

//! dviFontMapEntry - The information about a TeX font given by a line of the TeX font map psfonts.map, e.g.
//!   tcrm1000 SFRM1000 " TS1Encoding ReEncodeFont " <cm-super-ts1.enc <sfrm1000.pfb
//! meaning that the TeX font tcrm1000 is the type 1 font SFRM1000, from the file sfrm1000.pfb, re-encoded using the
//! encoding vector in cm-super-ts1.enc.
typedef struct dviFontMapEntry {
    char psName[SSTR_LENGTH]; // Postscript name of the type 1 font, or empty if not given
    char fontFile[SSTR_LENGTH]; // Filename of the type 1 font, or empty if it is resident in the printer
    char encFile[SSTR_LENGTH]; // Filename of the encoding vector to apply, or empty if none
    double slant; // Slant to apply to the font, from a "SlantFont" instruction (zero for none)
    double extend; // Horizontal scaling to apply to the font, from an "ExtendFont" instruction (one for none)
} dviFontMapEntry;

//! dviCopyToken - Copy a whitespace-delimited token into a buffer
//! \param [in] in - Pointer to the start of the token
//! \param [out] out - Buffer into which to write the token
//! \param [in] outLen - Size of the buffer <out>
//! \return Pointer to the first character after the token

static const char *dviCopyToken(const char *in, char *out, const size_t outLen) {
    size_t i = 0;
    while ((*in != '\0') && !isspace((unsigned char) *in)) {
        if (i < outLen - 1) out[i++] = *in;
        in++;
    }
    out[i] = '\0';
    return in;
}

//! dviParseFontMapLine - Parse a line of a dvips font map file
//! \param [in] line - The line, which must start with the TeX font name followed by whitespace
//! \param [out] entry - The information in the line
//! \return Zero on success, or nonzero if the line could not be parsed

static int dviParseFontMapLine(const char *line, dviFontMapEntry *entry) {
    char token[SSTR_LENGTH];
    double lastNumber = 0;
    memset(entry, 0, sizeof(dviFontMapEntry));
    entry->extend = 1;

    line = dviCopyToken(line, token, sizeof(token)); // Skip the TeX font name
    while (1) {
        while (isspace((unsigned char) *line)) line++;
        if (*line == '\0') break;
        if (*line == '"') {
            // Postscript instructions, e.g. " .167 SlantFont ". Encodings are applied using the encoding file.
            line++;
            while ((*line != '"') && (*line != '\0')) {
                while (isspace((unsigned char) *line)) line++;
                if ((*line == '"') || (*line == '\0')) break;
                line = dviCopyToken(line, token, sizeof(token));
                if (token[strlen(token) - 1] == '"') {
                    token[strlen(token) - 1] = '\0';
                    line--;
                }
                if (strcmp(token, "SlantFont") == 0) entry->slant = lastNumber;
                else if (strcmp(token, "ExtendFont") == 0) entry->extend = lastNumber;
                else lastNumber = strtod(token, NULL);
            }
            if (*line == '"') line++;
        } else if (*line == '<') {
            // A file to download: <file, <<file, <[file, or "< file". Files ending .enc are encoding vectors.
            char *dest;
            while ((*line == '<') || (*line == '[')) line++;
            while (isspace((unsigned char) *line)) line++;
            line = dviCopyToken(line, token, sizeof(token));
            if ((strlen(token) > 4) && (strcmp(token + strlen(token) - 4, ".enc") == 0)) dest = entry->encFile;
            else dest = entry->fontFile;
            snprintf(dest, SSTR_LENGTH, "%s", token);
        } else {
            // The postscript name of the font
            line = dviCopyToken(line, token, sizeof(token));
            if (entry->psName[0] == '\0') snprintf(entry->psName, SSTR_LENGTH, "%s", token);
        }
    }
    return 0;
}

//! dviLookupFontMap - Look up a TeX font in the dvips font map psfonts.map
//! \param [in] ec - The error-reporting context
//! \param [in] texName - The name of the TeX font, e.g. tcrm1000
//! \param [out] entry - The information about the font in the map
//! \return Zero on success, or DVIE_NOFONT if the font map could not be found or does not list the font

static int dviLookupFontMap(pplerr_context *ec, const char *texName, dviFontMapEntry *entry) {
    char line[LSTR_LENGTH];
    const size_t nameLen = strlen(texName);
    char *mapPath;
    FILE *fp;
    int found = 0;

    mapPath = ppl_kpse_wrap_find_map(ec, "psfonts.map");
    if (mapPath == NULL) {
        if (DEBUG) ppl_log(ec, "Could not find TeX font map psfonts.map");
        return DVIE_NOFONT;
    }
    if ((fp = fopen(mapPath, "r")) == NULL) return DVIE_NOFONT;
    while (fgets(line, sizeof(line), fp) != NULL) {
        // Find the line which starts with the font name. (Comment lines start with one of %#;* so never match.)
        if ((strncmp(line, texName, nameLen) != 0) || !isspace((unsigned char) line[nameLen])) continue;
        found = (dviParseFontMapLine(line, entry) == 0);
        break;
    }
    fclose(fp);
    if (DEBUG && found) {
        char msg[PPL_MSG_LENGTH];
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Font map entry for %s: psName <%s> fontFile <%s> encFile <%s> slant %g extend %g", texName,
                          entry->psName, entry->fontFile, entry->encFile, entry->slant, entry->extend);
        ppl_log(ec, msg);
    }
    return found ? 0 : DVIE_NOFONT;
}

//! dviReadEncoding - Read a postscript encoding vector from an .enc file, such as cm-super-ts1.enc
//! \param [in] ec - The error-reporting context
//! \param [in] encFile - The filename of the encoding file
//! \return Postscript code for the encoding vector, of the form [ /name1 /name2 ... ], allocated in the current
//! ppl_memAlloc context, or NULL on error

static char *dviReadEncoding(pplerr_context *ec, const char *encFile) {
    char errStr[SSTR_LENGTH];
    char name[SSTR_LENGTH];
    char *path, *out;
    FILE *fp;
    int c, inArray = 0, nNames = 0, lineLen = 0;
    size_t outLen = 0, outAlloc = 8192;

    snprintf(name, sizeof(name), "%s", encFile);
    path = ppl_kpse_wrap_find_enc(ec, name);
    if ((path == NULL) || ((fp = fopen(path, "r")) == NULL)) {
        snprintf(errStr, SSTR_LENGTH, "Could not find font encoding file '%s'.", encFile);
        ppl_error(ec, ERR_FILE, -1, -1, errStr);
        return NULL;
    }
    if ((out = (char *) ppl_memAlloc(outAlloc)) == NULL) {
        fclose(fp);
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }
    outLen = sprintf(out, "[");

    // The file contains /EncodingName [ /glyph0 /glyph1 ... /glyph255 ] def, possibly with % comments
    while ((c = getc(fp)) != EOF) {
        if (c == '%') {
            while ((c != '\n') && (c != EOF)) c = getc(fp);
        } else if (c == '[') inArray = 1;
        else if (c == ']') break;
        else if (inArray && (c == '/')) {
            int len = 0;
            while (((c = getc(fp)) != EOF) && !isspace(c) && (c != '/') && (c != ']') && (c != '%')) {
                if (len < (int) sizeof(name) - 1) name[len++] = (char) c;
            }
            name[len] = '\0';
            if (c != EOF) ungetc(c, fp);
            if (outLen + len + 4 >= outAlloc) break; // Malformed: far longer than 256 glyph names
            if (lineLen + len > 72) {
                out[outLen++] = '\n';
                lineLen = 0;
            }
            outLen += sprintf(out + outLen, " /%s", name);
            lineLen += len + 2;
            nNames++;
        }
    }
    fclose(fp);
    if (nNames != 256) {
        snprintf(errStr, SSTR_LENGTH, "Font encoding file '%s' contains %d glyph names, rather than 256.", encFile,
                 nNames);
        ppl_error(ec, ERR_FILE, -1, -1, errStr);
        return NULL;
    }
    sprintf(out + outLen, " ]");
    return out;
}

//! dviGetFontFromMap - Locate the type 1 font for a TeX font by looking it up in the dvips font map psfonts.map. If
//! the map says that the font must be re-encoded, slanted or extended, generate postscript code to do so.
//! \param [in] ec - The error-reporting context
//! \param [in] font - The font, whose <pfaPath>, <psName>, <basePsName> and <fontSetup> are set
//! \return Zero on success, DVIE_NOFONT if the font is not in the map, or another DVIE_* error code

static int dviGetFontFromMap(pplerr_context *ec, dviFontDetails *font) {
    dviFontMapEntry entry;
    char *encoding = NULL;
    char *setup;
    size_t setupLen;
    int err;

    if ((err = dviLookupFontMap(ec, font->name, &entry)) != 0) return err;

    if (entry.fontFile[0] != '\0') {
        // The font file is given with its extension, e.g. sfrm1000.pfb, but dviGetPfa() adds its own extension
        char fileStem[SSTR_LENGTH];
        char *dot;
        snprintf(fileStem, sizeof(fileStem), "%s", entry.fontFile);
        dot = strrchr(fileStem, '.');
        if ((dot != NULL) && ((strcmp(dot, ".pfb") == 0) || (strcmp(dot, ".pfa") == 0))) *dot = '\0';
        if ((err = dviGetPfa(ec, font, fileStem)) != 0) return err;
        font->basePsName = psNameFromPFA(ec, font->pfaPath);
        if (font->basePsName == NULL) return 1;
    } else if (entry.psName[0] != '\0') {
        // A font resident in the postscript interpreter, e.g. Times-Roman
        font->pfaPath = NULL;
        font->basePsName = (char *) ppl_memAlloc(strlen(entry.psName) + 1);
        if (font->basePsName == NULL) {
            ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
            return DVIE_MEMORY;
        }
        strcpy(font->basePsName, entry.psName);
    } else {
        return DVIE_NOFONT;
    }

    // If the font is used without modification, it needs no setup
    if ((entry.encFile[0] == '\0') && (entry.slant == 0) && (entry.extend == 1)) {
        font->psName = font->basePsName;
        return 0;
    }

    // Otherwise, define a modified copy of the font, named e.g. SFRM1000-tcrm1000
    if ((entry.encFile[0] != '\0') && ((encoding = dviReadEncoding(ec, entry.encFile)) == NULL)) return DVIE_NOFONT;
    font->psName = (char *) ppl_memAlloc(strlen(font->basePsName) + strlen(font->name) + 2);
    setupLen = 2 * strlen(font->basePsName) + strlen(font->name) + (encoding ? strlen(encoding) : 0) + 512;
    setup = (char *) ppl_memAlloc(setupLen);
    if ((font->psName == NULL) || (setup == NULL)) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    sprintf(font->psName, "%s-%s", font->basePsName, font->name);
    snprintf(setup, setupLen,
             "/%s /%s findfont\n"
             "dup length dict begin\n"
             "{1 index /FID ne {def} {pop pop} ifelse} forall\n"
             "%s%s%s"
             "/FontMatrix FontMatrix [%g 0 %g 1 0 0] matrix concatmatrix def\n"
             "currentdict end\n"
             "definefont pop\n",
             font->psName, font->basePsName, encoding ? "/Encoding " : "", encoding ? encoding : "",
             encoding ? " def\n" : "", entry.extend, entry.slant);
    font->fontSetup = setup;
    return 0;
}

//! psNameFromPFA - Extract the postscript name of a font from a PFA file, by finding its /FontName definition
//! \param [in] ec - The error-reporting context
//! \param [in] PFApath - The path of the PFA file
//! \return The postscript font name, allocated in the current ppl_memAlloc context, or NULL if it could not be found

char *psNameFromPFA(pplerr_context *ec, const char *PFApath) {
    FILE *fp;
    char *s;
    char buf[SSTR_LENGTH], c;
    int i;

    if ((fp = fopen(PFApath, "r")) == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Cannot open pfa file");
        return NULL;
    }

    // Forward file to the first space
    c = '\0';
    while (c != ' ') c = getc(fp);

    // Grab characters until the next space
    i = 0;
    while ((c = getc(fp)) != ' ' && i < SSTR_LENGTH - 1) {
        buf[i] = c;
        i++;
    }
    buf[i] = '\0';
    fclose(fp);

    // Produce a malloced string with the name in and return it
    s = (char *) ppl_memAlloc(i + 1);
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }
    snprintf(s, i + 1, "%s", buf);
    return s;
}

//! pfb2pfa - Convert a type 1 font from binary PFB format into ASCII PFA format
//! \param [in] ec - The error-reporting context
//! \param [in] in - The PFB file to read
//! \param [in] out - The PFA file to write
//! \return Zero on success, or a DVIE_* error code

int pfb2pfa(pplerr_context *ec, FILE *in, FILE *out) {
    int i, j; // Loop variables
    int len; // Record length
    char c; // Input string

    while (!feof(in)) {
        if (getc(in) != 128) {
            ppl_error(ec, ERR_INTERNAL, -1, -1, "Error in pfb file format");
            return 1;
        }
        i = getc(in);
        if (i == 1) {
            // Ascii text record
            len = getc(in) | getc(in) << 8 | getc(in) << 16 | getc(in) << 24;
            for (j = 0; j < len; j++) {
                if ((c = getc(in)) == '\r') putc('\n', out);
                else putc(c, out);
            }
        } else if (i == 2) {
            // Binary data record
            len = getc(in) | getc(in) << 8 | getc(in) << 16 | getc(in) << 24;
            for (j = 0; j < len; j++) {
                fprintf(out, "%02x", getc(in));
                if ((j + 1) % 30 == 0) putc('\n', out);
            }
            putc('\n', out);
        } else if (i == 3) {
            // EOF
            break;
        } else {
            ppl_error(ec, ERR_INTERNAL, -1, -1, "Corrupt pfb file");
            return 1;
        }
    }
    return 0;
}

//! dviReadTFM - Read a TeX font metric (TFM) file
//! \param [in] ec - The error-reporting context
//! \param [in] fp - The file handle of the TFM file
//! \param [out] err - Set to zero on success, or a DVIE_* error code
//! \return The font metrics, allocated in the current ppl_memAlloc context, or NULL on failure

dviTFM *dviReadTFM(pplerr_context *ec, FILE *fp, int *err) {
    char msg[PPL_MSG_LENGTH];
    dviTFM *tfm;
    unsigned long int buff[12];
    int i, j;
    int lh;
    int Nchars;

    char *tit[12] = {"lf", "lh", "bc", "ec", "nw", "nh", "nd", "ni", "nl", "nk", "ne", "np"};

    tfm = (dviTFM *) ppl_memAlloc(sizeof(dviTFM));
    if (tfm == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }

    // Read the file header
    for (i = 0; i < 12; i++) ReadLongInt(ec, fp, buff + i, 2);
    tfm->lf = buff[0];
    tfm->lh = buff[1];
    tfm->bc = buff[2];
    tfm->ec = buff[3];
    tfm->nw = buff[4];
    tfm->nh = buff[5];
    tfm->nd = buff[6];
    tfm->ni = buff[7];
    tfm->nl = buff[8];
    tfm->nk = buff[9];
    tfm->ne = buff[10];
    tfm->np = buff[11];

    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: ");
        j = strlen(msg);
        for (i = 0; i < 12; i++) {
            ppl_snprintfTrunc(msg + j, sizeof(msg) - (j), "%s:%lu  ", tit[i], buff[i]);
            j += strlen(msg + j);
        }
        ppl_log(ec, msg);
    }

    // We should have lf=6+lh+(ec-bc+1)+nw+nh+nd+ni+nl+nk+ne+np
    if (tfm->lf !=
        6 + tfm->lh + tfm->ec - tfm->bc + 1 + tfm->nw + tfm->nh + tfm->nd + tfm->ni + tfm->nl + tfm->nk + tfm->ne +
                tfm->np) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "TFM fail");
        *err = 1;
        return NULL;
    }

    // Read the header (distinct from the file header...)
    lh = tfm->lh;
    ReadLongInt(ec, fp, buff, 4);
    tfm->checksum = buff[0];
    lh--;
    tfm->ds = ReadFixWord(ec, fp, err);
    if (*err) return NULL;
    lh--;
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: lh now %d", lh);
        ppl_log(ec, msg);
    }
    if (lh >= 10) {
        int len;
        if ((*err = ReadUChar(ec, fp, &len)) != 0) return NULL;
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "TFM: Coding length: %d", len);
            ppl_log(ec, msg);
        }
        if (len > 39) {
            ppl_error(ec, ERR_INTERNAL, -1, -1, "Malformed DVI header. coding len>40");
            len = 39;
        }
        for (i = 0; i < 39; i++) {
            int t;
            if ((*err = ReadUChar(ec, fp, &t)) != 0) return NULL;
            tfm->coding[i] = t;
        }
        tfm->coding[len] = '\0';
        lh -= 10;
    }
    if (lh >= 5) {
        int len;
        if ((*err = ReadUChar(ec, fp, &len)) != 0) return NULL;
        if (len > 19) {
            ppl_error(ec, ERR_INTERNAL, -1, -1, "Malformed DVI header. coding len>19");
            len = 19;
        }
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "TFM: Family length: %d", len);
            ppl_log(ec, msg);
        }
        for (i = 0; i < 19; i++) {
            int t;
            if ((*err = ReadUChar(ec, fp, &t)) != 0) return NULL;
            tfm->family[i] = t;
        }
        tfm->family[len] = '\0';
        lh -= 5;
    }
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: coding:%s: family:%s: lh now %d", tfm->coding, tfm->family, lh);
        ppl_log(ec, msg);
    }
    if (lh > 0) {
        int temp;
        if ((*err = ReadUChar(ec, fp, &temp)) != 0) return NULL;
        if ((*err = ReadUChar(ec, fp, &temp)) != 0) return NULL;
        tfm->face = temp;
        if ((*err = ReadUChar(ec, fp, &temp)) != 0) return NULL;
        if ((*err = ReadUChar(ec, fp, &temp)) != 0) return NULL;
        lh--;
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "TFM: face:%d", tfm->face);
            ppl_log(ec, msg);
        }
    }
    while (lh > 0) {
        unsigned long int i;
        ReadLongInt(ec, fp, &i, 4);
        lh--;
    }

    // Read the char info tables
    Nchars = tfm->ec - tfm->bc + 1;
    tfm->charInfo = (TFMcharInfo *) ppl_memAlloc(Nchars * sizeof(TFMcharInfo));
    if (tfm->charInfo == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }
    for (i = 0; i < Nchars; i++) {
        int j;
        int t[4];
        for (j = 0; j < 4; j++)
            if ((*err = ReadUChar(ec, fp, t + j)) != 0) return NULL;
        (tfm->charInfo + i)->wi = t[0];
        (tfm->charInfo + i)->hi = t[1] >> 4;
        (tfm->charInfo + i)->di = t[1] & 0xf;
        (tfm->charInfo + i)->ii = t[2] >> 2;
        (tfm->charInfo + i)->tag = t[2] & 0x3;
        (tfm->charInfo + i)->rem = t[3];
    }

    // Read the width, height, depth & italic tables
    tfm->width = (double *) ppl_memAlloc(tfm->nw * sizeof(double));
    tfm->height = (double *) ppl_memAlloc(tfm->nh * sizeof(double));
    tfm->depth = (double *) ppl_memAlloc(tfm->nd * sizeof(double));
    tfm->italic = (double *) ppl_memAlloc(tfm->ni * sizeof(double));
    if ((tfm->width == NULL) || (tfm->height == NULL) || (tfm->depth == NULL) || (tfm->italic == NULL)) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }

    for (i = 0; i < tfm->nw; i++) {
        tfm->width[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }
    for (i = 0; i < tfm->nh; i++) {
        tfm->height[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }
    for (i = 0; i < tfm->nd; i++) {
        tfm->depth[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }
    for (i = 0; i < tfm->ni; i++) {
        tfm->italic[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }

    // Read the lig_kern table
    tfm->ligKern = (TFMligKern *) ppl_memAlloc(tfm->nl * sizeof(TFMligKern));
    if (tfm->ligKern == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }
    for (i = 0; i < tfm->nl; i++) {
        int j;
        if ((*err = ReadUChar(ec, fp, &j)) != 0) return NULL;
        (tfm->ligKern + i)->skip_byte = j;
        if ((*err = ReadUChar(ec, fp, &j)) != 0) return NULL;
        (tfm->ligKern + i)->next_char = j;
        if ((*err = ReadUChar(ec, fp, &j)) != 0) return NULL;
        (tfm->ligKern + i)->op_byte = j;
        if ((*err = ReadUChar(ec, fp, &j)) != 0) return NULL;
        (tfm->ligKern + i)->remainder = j;
    }

    // Read the kern table
    tfm->kern = (double *) ppl_memAlloc(tfm->nk * sizeof(double));
    if (tfm->kern == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }
    for (i = 0; i < tfm->nk; i++) {
        tfm->kern[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }

    // Read the extensible character recipies
    tfm->extensibleRecipe = (TFMextRec *) ppl_memAlloc(tfm->ne * sizeof(TFMextRec));
    if (tfm->extensibleRecipe == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }
    for (i = 0; i < tfm->ne; i++) {
        int j;
        if ((*err = ReadUChar(ec, fp, &j)) != 0) return NULL;
        (tfm->extensibleRecipe + i)->top = j;
        if ((*err = ReadUChar(ec, fp, &j)) != 0) return NULL;
        (tfm->extensibleRecipe + i)->mid = j;
        if ((*err = ReadUChar(ec, fp, &j)) != 0) return NULL;
        (tfm->extensibleRecipe + i)->bot = j;
        if ((*err = ReadUChar(ec, fp, &j)) != 0) return NULL;
        (tfm->extensibleRecipe + i)->rep = j;
    }

    // We should read in the param aray at this point, but let's see if we can get away without doing so
    return tfm;
}

//! ReadFixWord - Read a TFM "fix_word", a signed four-byte fixed-point number with the binary point after 12 bits
//! \param [in] ec - The error-reporting context
//! \param [in] fp - The file handle of the TFM file
//! \param [out] err - Set to a non-zero value if the read failed
//! \return The value of the fix_word

double ReadFixWord(pplerr_context *ec, FILE *fp, int *err) {
    double fw;
    long signed int li;
    const double twoP20 = 1048576; // 2**20
    if ((*err = ReadSignedInt(ec, fp, &li, 4)) != 0) return 0.0;
    fw = (double) li / twoP20;
    return fw;
}

//! dviFindMaxSize - Find the maximum height and depth of the capital letters A-Z in a font, used to give text items a
//! consistent height
//! \param [in] ec - The error-reporting context
//! \param [in] font - The font
//! \return Zero on success, or a DVIE_* error code

int dviFindMaxSize(pplerr_context *ec, dviFontDetails *font) {
    char msg[PPL_MSG_LENGTH];
    dviTFM *tfm; // Details of this font
    int hi, di; // Indices
    int chnum; // Character number in this font
    TFMcharInfo *chin; // Character info for this character
    int i, loopMax, hmax, dmax;
    double height, depth;

    if (!font) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Internal font failure!");
        return DVIE_INTERNAL;
    }
    tfm = font->tfm;
    font->maxHeight = 0.0;
    font->maxDepth = 0.0;

    // Loop over upper-case characters
    loopMax = tfm->ec > ASCII_CHAR_Z_UP ? ASCII_CHAR_Z_UP : tfm->ec;
    hmax = 0;
    dmax = 0;
    for (i = ASCII_CHAR_A_UP; i <= loopMax; i++) {
        chnum = i - tfm->bc;
        chin = tfm->charInfo + chnum;
        hi = (int) chin->hi;
        di = (int) chin->di;
        height = tfm->height[hi];
        depth = tfm->depth[di];
        if (font->maxHeight < height) {
            font->maxHeight = height;
            hmax = i;
        }
        if (font->maxDepth < depth) {
            font->maxDepth = depth;
            dmax = i;
        }
    }

    // Loop over lower-case characters
    loopMax = tfm->ec > ASCII_CHAR_Z_DN ? ASCII_CHAR_Z_DN : tfm->ec;
    for (i = ASCII_CHAR_A_DN; i <= loopMax; i++) {
        chnum = i - tfm->bc;
        chin = tfm->charInfo + chnum;
        hi = (int) chin->hi;
        di = (int) chin->di;
        height = tfm->height[hi];
        depth = tfm->depth[di];
        if (font->maxHeight < height) {
            font->maxHeight = height;
            hmax = i;
        }
        if (font->maxDepth < depth) {
            font->maxDepth = depth;
            dmax = i;
        }
    }
    font->maxHeight *= font->useSize;
    font->maxDepth *= font->useSize;
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: Maximum height %f depth %f from characters %d %d %s %s",
                          font->maxHeight, font->maxDepth, hmax, dmax, (char *) (&hmax), (char *) (&dmax));
        ppl_log(ec, msg);
    }
    return 0;
}
