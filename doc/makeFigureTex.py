#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# makeFigureTex.py
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
Turn the example scripts in examples/ex_*.ppl into LaTeX source, for inclusion in the Pyxplot manual. Each part of
a script between lines reading "# BEGIN" and "# END" becomes a separate file examples/tex/<name>_<n>.tex.
"""

import glob
import logging
import os
import re
import shutil

from typing import Final, List, Tuple

# The directory into which the LaTeX files are written
TEX_DIR: Final[str] = "examples/tex"

# Regular expression substitutions which escape characters which are special in LaTeX. A backslash is first replaced
# by a placeholder, so that the backslashes inserted by later substitutions are not themselves escaped.
LATEX_ESCAPES: Final[List[Tuple[str, str]]] = [
    (r'[\\]', r'gpzywxqqq'),
    (r'[_]', r'\\_'),
    (r'[&]', r'\\&'),
    (r'[%]', r'\\%'),
    (r'[$]', r'\\$'),
    (r'[{]', r'\\{'),
    (r'[}]', r'\\}'),
    (r'[#]', r'\\#'),
    (r'[\^]', r'\\^{}'),
    (r'[~]', r'$\\sim$'),
    (r'[<]', r'$<$'),  # < and > are only available in math mode
    (r'[>]', r'$>$'),
    (r'gpzywxqqq', r'$\\backslash$'),
    (r' ', r'~'),  # Preserve spacing, including indentation
]


def line_texify(line: str) -> str:
    """
    Convert a line of an example script into LaTeX, removing the paths of files, and escaping special characters.

    :param line:
        The line of the example script
    :return:
        LaTeX source to typeset the line
    """

    # Lines starting #NC are not comments, but commands which are commented out in the example
    if line.startswith("#NC "):
        line = line[4:]

    # Remove the paths of files used by examples
    line = re.sub(r'examples/eps/ex_', '', line)
    line = re.sub(r'examples/ex_', '', line)
    line = re.sub(r'examples/', '', line)

    for pattern, replacement in LATEX_ESCAPES:
        line = re.sub(pattern, replacement, line)
    return line


def make_tex(filename: str, counter: int, lines: List[str]) -> None:
    """
    Write a LaTeX file containing a part of an example script, in a typewriter font.

    :param filename:
        The filename of the example script
    :param counter:
        The number of this part of the example script, counting from one
    :param lines:
        The lines of this part of the example script
    :return:
        None
    """

    out_filename: Final[str] = os.path.join(TEX_DIR, "{}_{:d}.tex".format(os.path.split(filename)[1][:-4], counter))

    # Long lines are set in a smaller font
    footnote_size: Final[bool] = max(len(line) for line in lines) > 64

    with open(out_filename, "w") as output:
        if footnote_size:
            output.write("{\\footnotesize\n")
        first: bool = True
        for line in lines:
            if not first:
                output.write("\\newline\n")

            # Blank lines start a new paragraph
            if len(line.strip()) == 0:
                output.write("}\\\\{\\footnotesize\n" if footnote_size else "\\\\\n")
                first = True
                continue

            first = False
            output.write("\\noindent\\texttt{{{}}}".format(line_texify(line).strip()))
        if footnote_size:
            output.write("\n}")


def make_figure_tex() -> None:
    """
    Turn the parts of all the example scripts between "# BEGIN" and "# END" into LaTeX files.

    :return:
        None
    """

    # Start with an empty output directory
    shutil.rmtree(TEX_DIR, ignore_errors=True)
    os.mkdir(TEX_DIR)

    for filename in sorted(glob.glob("examples/ex_*.ppl")):
        logging.info("Converting example to latex <{}>...".format(os.path.split(filename)[1]))
        buffer: List[str] = []
        buffering: bool = False
        counter: int = 1
        with open(filename) as f:
            for line in f:
                if line.strip() == "# BEGIN":
                    buffering = True
                    buffer = []
                elif line.strip() == "# END":
                    make_tex(filename=filename, counter=counter, lines=buffer)
                    counter += 1
                    buffering = False
                elif buffering:
                    buffer.append(line)


# Do it right away if we're run as a script
if __name__ == "__main__":
    # Ensure we're running in the doc directory
    our_path: Final[str] = os.path.split(os.path.abspath(__file__))[0]
    os.chdir(our_path)

    # Configure logging
    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s:%(filename)s:%(message)s",
                        datefmt="%d/%m/%Y %H:%M:%S")

    make_figure_tex()
