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

// Loads the fonts used in LaTeX's dvi output, for dvi_interpreter.c. For each TeX font, the TeX font metric (TFM) file
// is read to find the sizes of its characters, and the Type 1 font which draws it is located: either a font built into
// postscript interpreters, a PFA or PFB file of the same name, or the font named in the dvips font map psfonts.map,
// possibly re-encoded or slanted. PFB files are converted to PFA so that they can be embedded in the postscript output.
// Files are found using kpse_wrap.c.

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
    // Variables for finding and reading the TFM file
    char msg[PPL_MSG_LENGTH];
    char errStr[SSTR_LENGTH];
    int err = 0;

    // Get the TFM file
    char *s = (char *) ppl_memAlloc(strlen(font->name) + 5);
    // Failure to allocate memory is reported as an error
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    sprintf(s, "%s.tfm", font->name);
    char *TFMpath = ppl_kpse_wrap_find_tfm(ec, s);
    // Report a TFM file which cannot be found
    if (TFMpath == NULL) {
        snprintf(errStr, SSTR_LENGTH,
                 "Could not find TFM file '%s'. You may be able to fix this with a more complete installation of tex.",
                 s);
        ppl_error(ec, ERR_INTERNAL, -1, -1, errStr);
        return 1;
    }
    // Log the path in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Font file %s: TFM path: %s", font->name, TFMpath);
        ppl_log(ec, msg);
    }
    FILE *TFMfp = fopen(TFMpath, "r");
    // Report a TFM file which cannot be opened
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

    // Log the font type in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: font type %d", font->fontType);
        ppl_log(ec, msg);
    }

    // Deal with built-in fonts
    font->psName = NULL;
    // Fonts built into postscript interpreters need no font file
    for (int i = 0; i < N_BUILTIN_FONTS; i++) {
        if (strlen(font->name) == strlen(builtinFonts[i]) &&
            strncmp(font->name, builtinFonts[i], L_BUILTIN_FNAME) == 0) {
            font->pfaPath = NULL;
            font->psName = (char *) ppl_memAlloc(strlen(builtinFontNames[i]) + 1);
            // Failure to allocate memory is reported as an error
            if (font->psName == NULL) {
                ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
                return DVIE_MEMORY;
            }
            strcpy(font->psName, builtinFontNames[i]);
            // Log the font in debugging mode
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
        // Report a font for which no font file could be found
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

    // Done
    return 0;
}


//! dviGetPfa - Locate the type 1 font file for a font, in PFA or PFB format, and convert it to PFA format if necessary,
//! for embedding in postscript output
//! \param [in] ec - The error-reporting context
//! \param [in] font - The font, whose postscript name is set
//! \param [out] filename - Character buffer into which to write the path of the PFA file
//! \return Zero on success, or a DVIE_* error code

int dviGetPfa(pplerr_context *ec, dviFontDetails *font, char *filename) {
    // Variables for finding the font file
    char errStr[SSTR_LENGTH];
    int err;

    // The filename with the extension .pfa or .pfb
    char *s = (char *) ppl_memAlloc(strlen(filename) + 5);
    // Failure to allocate memory is reported as an error
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return DVIE_MEMORY;
    }
    sprintf(s, "%s.pfa", filename);
    // Crude lower-casing
    for (int i = 0; i < strlen(filename); i++)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = s[i] + 'a' - 'A';
    char *PFApath = ppl_kpse_wrap_find_pfa(ec, s);
    // Use a PFA file if there is one
    if (PFApath != NULL) {
        font->pfaPath = PFApath;
    } else {
        // Make a PFA file from the PFB file (assuming that one exists)
        sprintf(s, "%s.pfb", filename);
        // Crude lower-casing
        for (int i = 0; i < strlen(filename); i++)
            if (s[i] >= 'A' && s[i] <= 'Z') s[i] = s[i] + 'a' - 'A';
        char *PFBpath = ppl_kpse_wrap_find_pfb(ec, s);
        // Otherwise there must be a PFB file
        if (PFBpath == NULL) {
            snprintf(errStr, SSTR_LENGTH, "dviGetTfm: Cannot find pfa or pfb file for font %s using name %s",
                     font->name, filename);
            ppl_log(ec, errStr);
            return DVIE_NOFONT;
        }
        FILE *fpin = fopen(PFBpath, "r");
        // Report a PFB file which cannot be opened
        if (fpin == NULL) {
            snprintf(errStr, SSTR_LENGTH, "dviGetTfm: Cannot open pfb file %s", PFBpath);
            ppl_error(ec, ERR_GENERIC, -1, -1, errStr);
            return DVIE_ACCESS;
        }

        // Make a filename for the destination pfa file
        // free(PFApath);
        PFApath = (char *) ppl_memAlloc(SSTR_LENGTH);
        // Failure to allocate memory is reported as an error
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
        FILE *fpout = fopen(PFApath, "w");
        // Report a PFA file which cannot be written
        if (fpout == NULL) {
            snprintf(errStr, SSTR_LENGTH, "dviGetTfm: Cannot write to pfa file %s", PFApath);
            ppl_error(ec, ERR_GENERIC, -1, -1, errStr);
            fclose(fpin);
            return DVIE_ACCESS;
        }
        // Convert the PFB file to PFA
        if ((err = pfb2pfa(ec, fpin, fpout)) != 0) {
            // Log the failure in debugging mode
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
    // Copy characters until a space, truncating the token if the buffer is too short
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
    // Variables for parsing the line; numbers precede the postscript operator which they modify
    char token[SSTR_LENGTH];
    double lastNumber = 0;
    memset(entry, 0, sizeof(dviFontMapEntry));
    entry->extend = 1;

    line = dviCopyToken(line, token, sizeof(token)); // Skip the TeX font name
    // Process each remaining token: the postscript font name, quoted postscript instructions, and font and encoding
    // files prefixed with <
    while (1) {
        while (isspace((unsigned char) *line)) line++;
        if (*line == '\0') break;
        // Quoted postscript instructions, which may slant or extend the font
        if (*line == '"') {
            // Postscript instructions, e.g. " .167 SlantFont ". Encodings are applied using the encoding file.
            line++;
            // Read each token within the quotes
            while ((*line != '"') && (*line != '\0')) {
                while (isspace((unsigned char) *line)) line++;
                if ((*line == '"') || (*line == '\0')) break;
                line = dviCopyToken(line, token, sizeof(token));
                // The final token ends with the closing quote
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
    // Variables for reading the font map
    char line[LSTR_LENGTH];
    const size_t nameLen = strlen(texName);
    FILE *fp;
    int found = 0;

    // Find the font map
    char *mapPath = ppl_kpse_wrap_find_map(ec, "psfonts.map");
    // Without a font map, no fonts can be found in it
    if (mapPath == NULL) {
        if (DEBUG) ppl_log(ec, "Could not find TeX font map psfonts.map");
        return DVIE_NOFONT;
    }
    if ((fp = fopen(mapPath, "r")) == NULL) return DVIE_NOFONT;
    // Find the line which describes the font, skipping comments
    while (fgets(line, sizeof(line), fp) != NULL) {
        // Find the line which starts with the font name. (Comment lines start with one of %#;* so never match.)
        if ((strncmp(line, texName, nameLen) != 0) || !isspace((unsigned char) line[nameLen])) continue;
        found = (dviParseFontMapLine(line, entry) == 0);
        break;
    }
    fclose(fp);
    // Log what was found in debugging mode
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
    // Variables for reading the encoding file
    char errStr[SSTR_LENGTH];
    char name[SSTR_LENGTH];
    char *out;
    FILE *fp;
    int c, inArray = 0, nNames = 0, lineLen = 0;
    size_t outLen = 0, outAlloc = 8192;

    // Find and open the encoding file
    snprintf(name, sizeof(name), "%s", encFile);
    char *path = ppl_kpse_wrap_find_enc(ec, name);
    // Report an encoding file which cannot be found
    if ((path == NULL) || ((fp = fopen(path, "r")) == NULL)) {
        ppl_snprintfTrunc(errStr, SSTR_LENGTH, "Could not find font encoding file '%s'.", encFile);
        ppl_error(ec, ERR_FILE, -1, -1, errStr);
        return NULL;
    }
    // Failure to allocate memory is reported as an error
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
        // Copy each glyph name in the encoding's array, wrapping lines
        else if (inArray && (c == '/')) {
            int len = 0;
            while (((c = getc(fp)) != EOF) && !isspace(c) && (c != '/') && (c != ']') && (c != '%')) {
                if (len < (int) sizeof(name) - 1) name[len++] = (char) c;
            }
            name[len] = '\0';
            if (c != EOF) ungetc(c, fp);
            if (outLen + len + 4 >= outAlloc) break; // Malformed: far longer than 256 glyph names
            // Start a new line before lines become too long
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
    // An encoding must contain 256 glyph names
    if (nNames != 256) {
        ppl_snprintfTrunc(errStr, SSTR_LENGTH, "Font encoding file '%s' contains %d glyph names, rather than 256.",
                          encFile, nNames);
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
    // Variables for finding the font
    dviFontMapEntry entry;
    char *encoding = NULL;
    int err;

    // Find the font in the font map
    if ((err = dviLookupFontMap(ec, font->name, &entry)) != 0) return err;

    // The font may be in a font file, or resident in the postscript interpreter
    if (entry.fontFile[0] != '\0') {
        // The font file is given with its extension, e.g. sfrm1000.pfb, but dviGetPfa() adds its own extension
        char fileStem[SSTR_LENGTH];
        snprintf(fileStem, sizeof(fileStem), "%s", entry.fontFile);
        char *dot = strrchr(fileStem, '.');
        if ((dot != NULL) && ((strcmp(dot, ".pfb") == 0) || (strcmp(dot, ".pfa") == 0))) *dot = '\0';
        if ((err = dviGetPfa(ec, font, fileStem)) != 0) return err;
        font->basePsName = psNameFromPFA(ec, font->pfaPath);
        if (font->basePsName == NULL) return 1;
    } else if (entry.psName[0] != '\0') {
        // A font resident in the postscript interpreter, e.g. Times-Roman
        font->pfaPath = NULL;
        font->basePsName = (char *) ppl_memAlloc(strlen(entry.psName) + 1);
        // Failure to allocate memory is reported as an error
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
    size_t setupLen = 2 * strlen(font->basePsName) + strlen(font->name) + (encoding ? strlen(encoding) : 0) + 512;
    char *setup = (char *) ppl_memAlloc(setupLen);
    // Failure to allocate memory is reported as an error
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
    // Variables for reading the font file
    FILE *fp;
    char buf[SSTR_LENGTH];

    // Report a font file which cannot be opened
    if ((fp = fopen(PFApath, "r")) == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Cannot open pfa file");
        return NULL;
    }

    // Forward file to the first space
    char c = '\0';
    while (c != ' ') c = getc(fp);

    // Grab characters until the next space
    int i = 0;
    // Copy the font name, up to the next space
    while ((c = getc(fp)) != ' ' && i < SSTR_LENGTH - 1) {
        buf[i] = c;
        i++;
    }
    buf[i] = '\0';
    fclose(fp);

    // Produce a malloced string with the name in and return it
    char *s = (char *) ppl_memAlloc(i + 1);
    // Failure to allocate memory is reported as an error
    if (s == NULL) {
        ppl_error(ec, ERR_MEMORY, -1, -1, "Out of memory");
        return NULL;
    }
    snprintf(s, i + 1, "%s", buf);
    return s;
}

//! pfbReadLength - Read the length of a PFB record, which is a four-byte little-endian integer
//! \param [in] in - The PFB file to read
//! \return The length of the record

static int pfbReadLength(FILE *in) {
    // Read the bytes one at a time, since C does not fix the order in which the operands of | are evaluated
    const int b0 = getc(in);
    const int b1 = getc(in);
    const int b2 = getc(in);
    const int b3 = getc(in);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

//! pfb2pfa - Convert a type 1 font from binary PFB format into ASCII PFA format
//! \param [in] ec - The error-reporting context
//! \param [in] in - The PFB file to read
//! \param [in] out - The PFA file to write
//! \return Zero on success, or a DVIE_* error code

int pfb2pfa(pplerr_context *ec, FILE *in, FILE *out) {
    int len; // Record length
    char c; // Input string

    // A PFB file is a series of records, each starting with the byte 128 and a type: 1 for text, 2 for binary data, and
    // 3 for the end of the file
    while (!feof(in)) {
        // Every record starts with the byte 128
        if (getc(in) != 128) {
            ppl_error(ec, ERR_INTERNAL, -1, -1, "Error in pfb file format");
            return 1;
        }
        int i = getc(in); // Loop variables
        // Copy text records, converting carriage returns to newlines; write binary records in hexadecimal
        if (i == 1) {
            // Ascii text record
            len = pfbReadLength(in);
            // Copy each character of the text record
            for (int j = 0; j < len; j++) {
                if ((c = getc(in)) == '\r') putc('\n', out);
                else putc(c, out);
            }
        } else if (i == 2) {
            // Binary data record
            len = pfbReadLength(in);
            // Write each byte of the binary record as two hexadecimal digits
            for (int j = 0; j < len; j++) {
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
    // Variables for reading the TFM file
    char msg[PPL_MSG_LENGTH];
    unsigned long int buff[12];
    int i, j;

    // The names of the twelve lengths in the TFM file's header, for debugging
    char *tit[12] = {"lf", "lh", "bc", "ec", "nw", "nh", "nd", "ni", "nl", "nk", "ne", "np"};

    // Allocate the font metrics
    dviTFM *tfm = (dviTFM *) ppl_memAlloc(sizeof(dviTFM));
    // Failure to allocate memory is reported as an error
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

    // Log the header in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: ");
        j = strlen(msg);
        // Write each length
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
    int lh = tfm->lh;
    ReadLongInt(ec, fp, buff, 4);
    tfm->checksum = buff[0];
    lh--;
    tfm->ds = ReadFixWord(ec, fp, err);
    if (*err) return NULL;
    lh--;
    // Log progress in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: lh now %d", lh);
        ppl_log(ec, msg);
    }
    // If present, read the character coding scheme, as a string of up to 39 characters
    if (lh >= 10) {
        int len;
        if ((*err = ReadUChar(ec, fp, &len)) != 0) return NULL;
        // Log the length in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "TFM: Coding length: %d", len);
            ppl_log(ec, msg);
        }
        // The string may be no longer than 39 characters
        if (len > 39) {
            ppl_error(ec, ERR_INTERNAL, -1, -1, "Malformed DVI header. coding len>40");
            len = 39;
        }
        // Read the string, which occupies 39 bytes
        for (i = 0; i < 39; i++) {
            int t;
            if ((*err = ReadUChar(ec, fp, &t)) != 0) return NULL;
            tfm->coding[i] = t;
        }
        tfm->coding[len] = '\0';
        lh -= 10;
    }
    // If present, read the font family, as a string of up to 19 characters
    if (lh >= 5) {
        int len;
        if ((*err = ReadUChar(ec, fp, &len)) != 0) return NULL;
        // The string may be no longer than 19 characters
        if (len > 19) {
            ppl_error(ec, ERR_INTERNAL, -1, -1, "Malformed DVI header. coding len>19");
            len = 19;
        }
        // Log the length in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "TFM: Family length: %d", len);
            ppl_log(ec, msg);
        }
        // Read the string, which occupies 19 bytes
        for (i = 0; i < 19; i++) {
            int t;
            if ((*err = ReadUChar(ec, fp, &t)) != 0) return NULL;
            tfm->family[i] = t;
        }
        tfm->family[len] = '\0';
        lh -= 5;
    }
    // Log the header in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: coding:%s: family:%s: lh now %d", tfm->coding, tfm->family, lh);
        ppl_log(ec, msg);
    }
    // If present, read the seven-bit safe flag and the face code
    if (lh > 0) {
        int temp;
        // The word holds the seven-bit safe flag, two unused bytes, and the face code
        if ((*err = ReadUChar(ec, fp, &temp)) != 0) return NULL;
        if ((*err = ReadUChar(ec, fp, &temp)) != 0) return NULL;
        if ((*err = ReadUChar(ec, fp, &temp)) != 0) return NULL;
        if ((*err = ReadUChar(ec, fp, &temp)) != 0) return NULL;
        tfm->face = temp;
        lh--;
        // Log the face code in debugging mode
        if (DEBUG) {
            ppl_snprintfTrunc(msg, sizeof(msg), "TFM: face:%d", tfm->face);
            ppl_log(ec, msg);
        }
    }
    // Skip any remaining words of the header
    while (lh > 0) {
        unsigned long int i;
        ReadLongInt(ec, fp, &i, 4);
        lh--;
    }

    // Read the char info tables
    int Nchars = tfm->ec - tfm->bc + 1;
    tfm->charInfo = (TFMcharInfo *) ppl_memAlloc(Nchars * sizeof(TFMcharInfo));
    // Failure to allocate memory is reported as an error
    if (tfm->charInfo == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }
    // Read the information about each character: indices into the tables of widths, heights, depths and italic
    // corrections, and its ligature, kerning or extensible recipe
    for (i = 0; i < Nchars; i++) {
        int t[4];
        for (int j = 0; j < 4; j++)
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
    // Failure to allocate memory is reported as an error
    if ((tfm->width == NULL) || (tfm->height == NULL) || (tfm->depth == NULL) || (tfm->italic == NULL)) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }

    // Read the table of widths
    for (i = 0; i < tfm->nw; i++) {
        tfm->width[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }
    // Read the table of heights
    for (i = 0; i < tfm->nh; i++) {
        tfm->height[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }
    // Read the table of depths
    for (i = 0; i < tfm->nd; i++) {
        tfm->depth[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }
    // Read the table of italic corrections
    for (i = 0; i < tfm->ni; i++) {
        tfm->italic[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }

    // Read the lig_kern table
    tfm->ligKern = (TFMligKern *) ppl_memAlloc(tfm->nl * sizeof(TFMligKern));
    // Failure to allocate memory is reported as an error
    if (tfm->ligKern == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }
    // Read the ligature and kerning program
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
    // Failure to allocate memory is reported as an error
    if (tfm->kern == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }
    // Read the table of kerns
    for (i = 0; i < tfm->nk; i++) {
        tfm->kern[i] = ReadFixWord(ec, fp, err);
        if (*err) return NULL;
    }

    // Read the extensible character recipies
    tfm->extensibleRecipe = (TFMextRec *) ppl_memAlloc(tfm->ne * sizeof(TFMextRec));
    // Failure to allocate memory is reported as an error
    if (tfm->extensibleRecipe == NULL) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Out of memory");
        *err = DVIE_MEMORY;
        return NULL;
    }
    // Read the recipes for extensible characters
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
    long signed int li;
    const double twoP20 = 1048576; // 2**20
    if ((*err = ReadSignedInt(ec, fp, &li, 4)) != 0) return 0.0;
    double fw = (double) li / twoP20;
    return fw;
}

//! dviFindMaxSize - Find the maximum height and depth of the capital letters A-Z in a font, used to give text items a
//! consistent height
//! \param [in] ec - The error-reporting context
//! \param [in] font - The font
//! \return Zero on success, or a DVIE_* error code

int dviFindMaxSize(pplerr_context *ec, dviFontDetails *font) {
    // Variables for finding the size of the font's capital letters
    char msg[PPL_MSG_LENGTH];
    int hi, di; // Indices
    int chnum; // Character number in this font
    TFMcharInfo *chin; // Character info for this character
    double height, depth;

    // There must be a font
    if (!font) {
        ppl_error(ec, ERR_INTERNAL, -1, -1, "Internal font failure!");
        return DVIE_INTERNAL;
    }
    dviTFM *tfm = font->tfm; // Details of this font
    font->maxHeight = 0.0;
    font->maxDepth = 0.0;

    // Loop over upper-case characters
    int loopMax = tfm->ec > ASCII_CHAR_Z_UP ? ASCII_CHAR_Z_UP : tfm->ec;
    int hmax = 0;
    int dmax = 0;
    // Find the greatest height and depth among the upper-case letters
    for (int i = ASCII_CHAR_A_UP; i <= loopMax; i++) {
        chnum = i - tfm->bc;
        chin = tfm->charInfo + chnum;
        hi = (int) chin->hi;
        di = (int) chin->di;
        height = tfm->height[hi];
        depth = tfm->depth[di];
        // Keep the greatest height
        if (font->maxHeight < height) {
            font->maxHeight = height;
            hmax = i;
        }
        // Keep the greatest depth
        if (font->maxDepth < depth) {
            font->maxDepth = depth;
            dmax = i;
        }
    }

    // Loop over lower-case characters
    loopMax = tfm->ec > ASCII_CHAR_Z_DN ? ASCII_CHAR_Z_DN : tfm->ec;
    // Also among the lower-case letters
    for (int i = ASCII_CHAR_A_DN; i <= loopMax; i++) {
        chnum = i - tfm->bc;
        chin = tfm->charInfo + chnum;
        hi = (int) chin->hi;
        di = (int) chin->di;
        height = tfm->height[hi];
        depth = tfm->depth[di];
        // Keep the greatest height
        if (font->maxHeight < height) {
            font->maxHeight = height;
            hmax = i;
        }
        // Keep the greatest depth
        if (font->maxDepth < depth) {
            font->maxDepth = depth;
            dmax = i;
        }
    }
    font->maxHeight *= font->useSize;
    font->maxDepth *= font->useSize;
    // Log the sizes in debugging mode
    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "TFM: Maximum height %f depth %f from characters %d %d %s %s",
                          font->maxHeight, font->maxDepth, hmax, dmax, (char *) (&hmax), (char *) (&dmax));
        ppl_log(ec, msg);
    }
    return 0;
}
