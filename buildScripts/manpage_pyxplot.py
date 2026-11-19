#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# manpage_pyxplot.py
#
# The code in this file is part of Pyxplot
# <http://www.pyxplot.org.uk>
#
# Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
#               2008-2013 Ross Church
#
# $Id$
#
# Pyxplot is free software; you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later
# version.
#
# You should have received a copy of the GNU General Public License along with
# Pyxplot; if not, write to the Free Software Foundation, Inc., 51 Franklin
# Street, Fifth Floor, Boston, MA  02110-1301, USA

# ----------------------------------------------------------------------------

"""
Generate the manpage for Pyxplot, writing it to stdout.
"""

import argparse
import os
import sys

from typing import Final

# The template for the manpage, into which the description, documentation path and authors are substituted
MANPAGE_TEMPLATE: Final[str] = r"""
.\" pyxplot.1
.\"
.\" The manpage in this file is part of Pyxplot
.\" <http://www.pyxplot.org.uk>
.\"
.\" Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
.\"               2008-2013 Ross Church
.\"
.\" $Id$
.\"
.\" Pyxplot is free software; you can redistribute it and/or modify it under the
.\" terms of the GNU General Public License as published by the Free Software
.\" Foundation; either version 2 of the License, or (at your option) any later
.\" version.
.\"
.\" You should have received a copy of the GNU General Public License along with
.\" Pyxplot; if not, write to the Free Software Foundation, Inc., 51 Franklin
.\" Street, Fifth Floor, Boston, MA  02110-1301, USA
.\"
.\" ----------------------------------------------------------------------------

.\" Man page for pyxplot

.TH PYXPLOT 1
.SH NAME
pyxplot \- a multi-purpose command-line data processing, vector graphics and graph-plotting tool.
.SH SYNOPSIS
.B pyxplot
[file ...]
.SH DESCRIPTION
{description}
Full documentation can be found in:
{doc_path}
.SH COMMAND LINE OPTIONS
  \-h, \-\-help:       Display this help.
  \-v, \-\-version:    Display version number.
  \-q, \-\-quiet:      Turn off initial welcome message.
  \-V, \-\-verbose:    Turn on initial welcome message.
  \-c, \-\-color:      Use colored highlighting of output.
  \-m, \-\-monochrome: Turn off coloured highlighting.
.SH AUTHORS
{authors}.
.SH BUGS AND USER FORUMS
To report bugs in Pyxplot, or to meet other Pyxplot users in our forums, please
visit us on sourceforge: <http://sourceforge.net/projects/pyxplot/>.
.SH CREDITS
Matthew Smith, Michael Rutter, Zoltan Voros and John Walker have all
contributed code to Pyxplot.  We welcome bug reports, which can be submitted to
our project page on Sourceforge, and thank the many testers who have already
made significant contributions to the project by helping us to track down bugs.
.SH "SEE ALSO"
.BR pyxplot_watch (1), gnuplot (1)
"""


def read_description(readme_filename: str) -> str:
    """
    Read the description of Pyxplot from section 1 of the README file.

    :param readme_filename:
        The filename of the README file
    :return:
        The text of section 1, excluding its heading
    """

    description: str = ""
    section: int = 0
    with open(readme_filename, "r") as f:
        for line in f:
            if line[0:2] == "1.":
                section = 1
            elif line[0:2] == "2.":
                section = 2
            elif section == 1:
                description += line
    return description


def make_manpage(doc_path: str) -> str:
    """
    Make the manpage for Pyxplot.

    :param doc_path:
        The path of Pyxplot's full documentation, which the manpage refers readers to
    :return:
        The text of the manpage
    """

    with open("AUTHORS", "r") as f:
        authors: Final[str] = f.read()
    description: Final[str] = read_description(readme_filename="README")
    return MANPAGE_TEMPLATE.format(description=description, doc_path=doc_path, authors=authors)


# Do it right away if we're run as a script
if __name__ == "__main__":
    # Read input parameters
    parser: Final[argparse.ArgumentParser] = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('doc_path', help="The path of Pyxplot's full documentation")
    args: Final[argparse.Namespace] = parser.parse_args()

    # Ensure we're running in the root of the Pyxplot source tree
    our_path: Final[str] = os.path.split(os.path.abspath(__file__))[0]
    os.chdir(os.path.join(our_path, ".."))

    sys.stdout.write(make_manpage(doc_path=args.doc_path))
