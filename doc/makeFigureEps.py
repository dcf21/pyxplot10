#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# makeFigureEps.py
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
Run the example scripts in examples/ex_*.ppl, which make the figures in the Pyxplot manual, and convert the EPS
figures which they produce into PDF format.
"""

import argparse
import glob
import logging
import os
import shutil
import subprocess

from typing import Final, List

# The directory into which the example scripts write their figures
EPS_DIR: Final[str] = "examples/eps"


def example_list(testing: bool) -> List[str]:
    """
    List the example scripts to run.

    :param testing:
        If true, list only the examples in examples.testlist, which are used to test Pyxplot
    :return:
        List of the filenames of the example scripts, in alphabetical order
    """

    if testing:
        with open("examples.testlist") as f:
            return sorted("examples/{}.ppl".format(item.strip()) for item in f)
    return sorted(glob.glob("examples/ex_*.ppl"))


def make_figures(pyxplot: str, testing: bool) -> None:
    """
    Run the example scripts which make the figures in the Pyxplot manual, and convert the figures to PDF format.

    :param pyxplot:
        The path of the Pyxplot binary to use
    :param testing:
        If true, only run the examples in examples.testlist, and do not convert the figures to PDF format
    :return:
        None
    """

    # Start with an empty output directory
    shutil.rmtree(EPS_DIR, ignore_errors=True)
    os.mkdir(EPS_DIR)

    # Run each of the example scripts
    for filename in example_list(testing=testing):
        logging.info("Working on example <{}>...".format(os.path.split(filename)[1]))
        if subprocess.run([pyxplot, filename]).returncode != 0:
            raise RuntimeError("pyxplot failed")

    # Make PDF versions of all figures
    if not testing:
        for eps in sorted(glob.glob(os.path.join(EPS_DIR, "*.eps"))):
            logging.info("Converting example <{}> to pdf...".format(os.path.split(eps)[1]))
            pdf: str = eps[:-4] + ".pdf"
            subprocess.run(["gs", "-dQUIET", "-dSAFER", "-P-", "-dBATCH", "-dNOPAUSE", "-dEPSCrop", "-sDEVICE=pdfwrite",
                            "-sOutputFile={}".format(pdf), eps])


# Do it right away if we're run as a script
if __name__ == "__main__":
    # Read input parameters
    parser: Final[argparse.ArgumentParser] = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pyxplot', nargs='?', default="../bin/pyxplot", help="The path of the Pyxplot binary to use")
    parser.add_argument('mode', nargs='?', choices=["test"],
                        help="Pass 'test' to run only the examples in examples.testlist, without making PDFs")
    args: Final[argparse.Namespace] = parser.parse_args()

    # Pyxplot may be given as a path relative to the current directory, which we are about to leave
    pyxplot_binary: Final[str] = os.path.abspath(args.pyxplot) if os.sep in args.pyxplot else args.pyxplot

    # Ensure we're running in the doc directory
    our_path: Final[str] = os.path.split(os.path.abspath(__file__))[0]
    os.chdir(our_path)

    # Configure logging
    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s:%(filename)s:%(message)s",
                        datefmt="%d/%m/%Y %H:%M:%S")

    make_figures(pyxplot=pyxplot_binary, testing=(args.mode == "test"))
