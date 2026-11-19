#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# makeFragmentTex.py
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
Turn the fragments of Pyxplot sessions in fragments/*.ppl into LaTeX source, for inclusion in the Pyxplot manual.
Each fragment is run through Pyxplot one line at a time, and the LaTeX shows each command, as if typed at the
pyxplot> prompt, followed by the output it produces.
"""

import argparse
import glob
import logging
import os
import re
import shutil
import subprocess

from typing import Final, List, Tuple

# The directory into which the LaTeX files are written
TEX_DIR: Final[str] = "fragments/tex"

# Regular expression substitutions which escape characters which are special in LaTeX, other than #. A backslash is
# first replaced by a placeholder, so that the backslashes inserted by later substitutions are not themselves escaped.
LATEX_ESCAPES: Final[List[Tuple[str, str]]] = [
    (r'[\\]', r'gpzywxqqq'),
    (r'[_]', r'\\_'),
    (r'[&]', r'\\&'),
    (r'[%]', r'\\%'),
    (r'[$]', r'\\$'),
    (r'[{]', r'\\{'),
    (r'[}]', r'\\}'),
    (r'[\^]', r'\\^{}'),
    (r'[~]', r'$\\sim$'),
    (r'[<]', r'$<$'),  # < and > are only available in math mode
    (r'[>]', r'$>$'),
    (r'gpzywxqqq', r'$\\backslash$'),
    (r' ', r'~'),  # Preserve spacing, including indentation
]


def line_texify(line: str, user_input: bool) -> str:
    """
    Convert a line of a Pyxplot session into LaTeX, escaping special characters.

    :param line:
        The line of text
    :param user_input:
        If true, the line is a command typed by the user, in which any comment after a # is set in italic. If false,
        the line is output from Pyxplot.
    :return:
        LaTeX source to typeset the line. For commands, this is placed inside two pairs of braces, and closes and
        reopens them to set the comment in italic.
    """

    for pattern, replacement in LATEX_ESCAPES:
        line = re.sub(pattern, replacement, line)

    if not user_input:
        return re.sub(r'[#]', r'\\#', line)

    # Set comments in italic
    hash_pos: Final[int] = line.find("#")
    if hash_pos < 0:
        return line
    return r"{}}}}}{{{{\it \#{}".format(line[:hash_pos], line[hash_pos + 1:])


def make_fragment_tex(pyxplot: str, filename: str) -> None:
    """
    Run a fragment of a Pyxplot session one line at a time, and write a LaTeX file showing each command followed by
    its output.

    :param pyxplot:
        The path of the Pyxplot binary to use
    :param filename:
        The filename of the fragment
    :return:
        None
    """

    out_filename: Final[str] = os.path.join(TEX_DIR, os.path.split(filename)[1][:-4] + ".tex")
    with open(filename) as f:
        lines: Final[List[str]] = f.readlines()

    with open(out_filename, "w") as out:
        output_line_count: int = 0  # The number of lines of output from the commands typeset so far
        first: bool = True
        prompt: str = "pyxplot"

        for index, line in enumerate(lines):
            if len(line.strip()) < 1:
                continue
            if not first:
                out.write("\\newline\n")
            first = False
            out.write(r"\noindent\texttt{{{}> \textbf{{{}}}}}".format(prompt, line_texify(line.rstrip(), True)))

            # A line ending in a backslash continues onto the next line
            if line.strip()[-1] == "\\":
                prompt = "......."
                continue
            prompt = "pyxplot"

            # Run all the commands so far through Pyxplot, and typeset any new output which this command produced
            result: subprocess.CompletedProcess[str] = subprocess.run(
                [pyxplot], input="\n".join([item.strip() for item in lines[0:index + 1]]),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='UTF-8')
            if len(result.stderr) > 0:
                raise RuntimeError("pyxplot failed: {}".format(result.stderr))
            output_lines: List[str] = result.stdout.strip().split('\n')
            if output_lines == ['']:
                output_lines = []
            if len(output_lines) <= output_line_count:
                continue
            new_lines: List[str] = output_lines[output_line_count:]
            output_line_count = len(output_lines)
            for output_line in new_lines:
                if len(output_line.strip()) < 1:
                    continue
                out.write("\\newline\n\\noindent\\texttt{{{}}}".format(line_texify(output_line, False)))
        out.write("\n")


def make_all_fragments_tex(pyxplot: str) -> None:
    """
    Turn all the fragments of Pyxplot sessions in fragments/*.ppl into LaTeX files.

    :param pyxplot:
        The path of the Pyxplot binary to use
    :return:
        None
    """

    # Start with an empty output directory
    shutil.rmtree(TEX_DIR, ignore_errors=True)
    os.mkdir(TEX_DIR)

    for filename in sorted(glob.glob("fragments/*.ppl")):
        logging.info("Converting fragment to latex <{}>...".format(os.path.split(filename)[1]))
        make_fragment_tex(pyxplot=pyxplot, filename=filename)


# Do it right away if we're run as a script
if __name__ == "__main__":
    # Read input parameters
    parser: Final[argparse.ArgumentParser] = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pyxplot', nargs='?', default="../bin/pyxplot", help="The path of the Pyxplot binary to use")
    args: Final[argparse.Namespace] = parser.parse_args()

    # Pyxplot may be given as a path relative to the current directory, which we are about to leave
    pyxplot_binary: Final[str] = os.path.abspath(args.pyxplot) if os.sep in args.pyxplot else args.pyxplot

    # Ensure we're running in the doc directory
    our_path: Final[str] = os.path.split(os.path.abspath(__file__))[0]
    os.chdir(our_path)

    # Configure logging
    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s:%(filename)s:%(message)s",
                        datefmt="%d/%m/%Y %H:%M:%S")

    make_all_fragments_tex(pyxplot=pyxplot_binary)
