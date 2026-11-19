#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# pyx_colors.py
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
Produce figures for the Pyxplot manual showing all of the named colors defined in PyX: arranged by hue and saturation
(pyx_colors), in alphabetical order (pyx_colors2), and in order of hue (pyx_colors3). Each is written in EPS and PDF
formats. This script requires the PyX package.
"""

import logging
import math
import os

from typing import Any, Dict, Final, List, Tuple

from pyx import canvas, color, path, text

# The radius of the circle on which hue is plotted in the first figure
WHEEL_RADIUS: Final[float] = 7.5

# The radius of the blob of color drawn for each color
BLOB_RADIUS: Final[float] = 0.2

# In the lists of colors, the number of colors in each column, and the spacing of rows and columns
COLUMN_LENGTH: Final[int] = 20
ROW_SPACING: Final[float] = 0.5
COLUMN_SPACING: Final[float] = 3.5


def named_colors() -> Dict[str, color.cmyk]:
    """
    Fetch all of the named colors defined in PyX.

    :return:
        Dictionary of colors, indexed by name
    """

    return {name: item for name, item in color.cmyk.__dict__.items() if isinstance(item, color.cmyk)}


def write_figure(c: canvas.canvas, name: str) -> None:
    """
    Write a figure in EPS and PDF formats.

    :param c:
        The canvas containing the figure
    :param name:
        The filename of the figure, without extension
    :return:
        None
    """

    c.writeEPSfile("{}.eps".format(name))
    c.writePDFfile("{}.pdf".format(name))


def color_wheel(colors: Dict[str, color.cmyk]) -> None:
    """
    Draw a figure showing the colors arranged by hue (angle) and saturation (radius), and write it to pyx_colors.eps
    and pyx_colors.pdf. Colors whose labels cannot be placed without overlapping other labels are omitted, and
    listed in the log.

    :param colors:
        Dictionary of colors, indexed by name
    :return:
        None
    """

    c: Final[canvas.canvas] = canvas.canvas()
    c.stroke(path.circle(0, 0, WHEEL_RADIUS))  # Draw the hue axis around the edge

    # Convert hue/saturation into x/y coordinates
    items: Final[List[Tuple[float, float, str]]] = []
    for name, item in colors.items():
        hsb: color.hsb = item.hsb()
        hue: float = hsb.h * 2 * math.pi
        items.append((hsb.s * math.sin(hue) * WHEEL_RADIUS, hsb.s * math.cos(hue) * WHEEL_RADIUS, name))

    # Draw from right to left
    items.sort(key=lambda entry: entry[0], reverse=True)

    # Try putting each label in each of these positions, until we find one which doesn't overlap other labels. Labels
    # on the left side of the figure prefer to sit to the left of their blobs, and vice versa.
    positions_l: Final[List[List[Any]]] = [[text.halign.boxleft, text.valign.top],
                                           [text.halign.boxleft, text.valign.bottom]]
    positions_r: Final[List[List[Any]]] = [[text.halign.boxright, text.valign.top],
                                           [text.halign.boxright, text.valign.bottom]]

    labels: Final[List[Any]] = []
    for x, y, name in items:
        positions: List[List[Any]] = (positions_l + positions_r) if x < 0 else (positions_r + positions_l)
        placed: bool = False
        label: Any = None
        for position in positions:
            label = text.text(x, y, name, position)
            if not any(label.bbox().intersects(other.bbox()) for other in labels):
                placed = True
                break
        if placed:
            c.fill(path.circle(x, y, BLOB_RADIUS), [colors[name]])  # Draw a blob of the appropriate color
            labels.append(label)
        else:
            logging.info("Could not place label for color <{}>".format(name))

    # ... and then label them
    for label in labels:
        c.insert(label)

    write_figure(c=c, name="pyx_colors")


def color_columns(names: List[str], colors: Dict[str, color.cmyk], filename: str) -> None:
    """
    Draw a figure listing colors in columns, with a blob of each color next to its name.

    :param names:
        The names of the colors, in the order in which they are to be listed
    :param colors:
        Dictionary of colors, indexed by name
    :param filename:
        The filename of the figure, without extension
    :return:
        None
    """

    c: Final[canvas.canvas] = canvas.canvas()
    for index, name in enumerate(names):
        y: float = -(index % COLUMN_LENGTH) * ROW_SPACING
        x: float = int(index / COLUMN_LENGTH) * COLUMN_SPACING
        c.fill(path.circle(x, y, BLOB_RADIUS), [colors[name]])
        c.text(x + 0.5, y, name, [text.halign.boxleft, text.valign.middle])
    write_figure(c=c, name=filename)


def make_color_figures() -> None:
    """
    Produce all three figures showing the named colors defined in PyX.

    :return:
        None
    """

    colors: Final[Dict[str, color.cmyk]] = named_colors()

    # Figure 1: Colors arranged in hue/saturation space
    color_wheel(colors=colors)

    # Figure 2: A list of colors, sorted alphabetically
    color_columns(names=sorted(colors.keys()), colors=colors, filename="pyx_colors2")

    # Figure 3: A list of colors, sorted by hue
    by_hue: Final[List[str]] = sorted(colors.keys(), key=lambda name: colors[name].hsb().h, reverse=True)
    color_columns(names=by_hue, colors=colors, filename="pyx_colors3")


# Do it right away if we're run as a script
if __name__ == "__main__":
    # Ensure we're running in the doc/figures directory
    our_path: Final[str] = os.path.split(os.path.abspath(__file__))[0]
    os.chdir(our_path)

    # Configure logging
    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s:%(filename)s:%(message)s",
                        datefmt="%d/%m/%Y %H:%M:%S")
    logging.getLogger("pyx").setLevel(logging.WARNING)  # PyX logs every call it makes to TeX at INFO level

    make_color_figures()
