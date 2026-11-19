// ppl_passwd.c
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

// Functions for making backup copies of files before they are overwritten

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "coreUtils/errorReport.h"
#include "pplConstants.h"
#include "settings/settingTypes.h"
#include "userspace/context.h"

//! ppl_createBackupIfRequired - If the user has enabled backups, and a file we are about to overwrite already exists,
//! rename the existing file to <filename~N>, where N is the lowest integer for which no such file already exists.
//! \param [in] c - The Pyxplot context, used to determine whether backups are enabled
//! \param [in] filename - The path of the file which is about to be overwritten

void ppl_createBackupIfRequired(const ppl_context *c, const char *filename) {
    char newname[FNAME_LENGTH];
    int i, j;

    if (c->set->term_current.backup == SW_ONOFF_OFF) return; // Backup is switched off
    if (access(filename, F_OK) != 0) return; // File we're about to write to does not already exist

    strcpy(newname, filename);
    i = strlen(filename);
    for (j = 0; j < 65536; j++) {
        sprintf(newname + i, "~%d", j);
        if (access(newname, F_OK) != 0) break; // We've found a backup file which does not already exist
    }
    rename(filename, newname);
    return;
}
