#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# manpage_pyxplot_watch.py
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
Generate the manpage for pyxplot_watch, writing it to stdout.
"""

import argparse
import os
import sys

from typing import Final

# The template for the manpage, into which the documentation path and authors are substituted
MANPAGE_TEMPLATE: Final[str] = r"""
.\" pyxplot_watch.1
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

.\" Man page for pyxplot_watch

.TH PYXPLOT_WATCH 1
.SH NAME
pyxplot_watch \- a tool which monitors a collection of Pyxplot command scripts
and executes them whenever they are modified.
.SH SYNOPSIS
.B pyxplot_watch
[file ...]
.SH DESCRIPTION
pyxplot_watch is a part of the Pyxplot plotting package; it is a simple tool
for watching Pyxplot command script files, and executing them whenever they are
modified. It is should be followed on the commandline by a list of command
scripts which are to be watched.  Full documentation can be found in:
{doc_path}
.SH COMMAND LINE OPTIONS
  \-v, \-\-verbose: Verbose mode; output full activity log to terminal
  \-q, \-\-quiet  : Quiet mode; only output Pyxplot error messages to terminal
  \-h, \-\-help   : Display this help
  \-V, \-\-version: Display version number
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
.BR pyxplot (1), gnuplot (1)
"""


def make_manpage(doc_path: str) -> str:
    """
    Make the manpage for pyxplot_watch.

    :param doc_path:
        The path of Pyxplot's full documentation, which the manpage refers readers to
    :return:
        The text of the manpage
    """

    with open("AUTHORS", "r") as f:
        authors: Final[str] = f.read()
    return MANPAGE_TEMPLATE.format(doc_path=doc_path, authors=authors)


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
