#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# parser_data.py
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
Generate the C source files src/parser/cmdList.h and src/parser/cmdList.c from the grammar of Pyxplot's commands in
buildScripts/parser_data.dat.

Each line of parser_data.dat defines the syntax of one command, as a sequence of words. Each word either matches some
text in the command (e.g. a keyword, or a %-prefixed item such as an expression), or is a grammar character such as
"[" which begins a list of repeated items. Words which match text may store what they match in a named output
variable, e.g. "plot@1:directive" matches the keyword "plot" (which may be abbreviated to one letter), and stores it
in the variable "directive".

The output variables of each command are stored in numbered slots. cmdList.c contains the grammar, in which each
variable name is replaced by its slot number. cmdList.h contains #defines of the slot numbers, of the form
PARSE_<directive>_<set option>_<variable name>.
"""

import logging
import os

from typing import Dict, Final, List, TextIO

# Grammar characters, which are passed straight through to the output
GRAMMAR_CHARACTERS: Final[List[str]] = ["=", "(", "~", ")", "{", "}", "<", "|", ">", "["]

# The output variables which every command has, and their slot numbers
STANDARD_VARIABLES: Final[Dict[str, int]] = {"directive": 0, "editno": 1, "set_option": 2, "X": 3}


def sanitize(in_str: str) -> str:
    """
    Remove all characters from a string, except for letters, digits and underscores, to make it usable in a C
    identifier.

    :param in_str:
        The string to sanitize
    :return:
        The sanitized string
    """

    return "".join(char for char in in_str if char.isalnum() or char == '_')


def process_var_table(var_table: Dict[str, int], directive: str, set_option: str,
                      global_var_names: List[str], global_var_table: Dict[str, Dict[str, int]]) -> None:
    """
    Record the slot numbers of the output variables of a command, so that write_var_tables() can later write a table
    of the slot number of every variable in each command.

    :param var_table:
        Dictionary of the slot numbers of the command's output variables, indexed by variable name
    :param directive:
        The name of the command, e.g. "plot"
    :param set_option:
        For the set command, the name of the setting followed by an underscore, e.g. "terminal_"; otherwise blank
    :param global_var_names:
        List of the names of all the output variables used by any command, to which this command's variables are added
    :param global_var_table:
        Dictionary of the variable tables of all commands, indexed by <directive>_<set option>, to which this
        command's table is added
    :return:
        None
    """

    if '%' in directive:
        return
    key: Final[str] = "{}_{}".format(directive, set_option)
    global_var_table[key] = var_table
    for name in var_table:
        if name not in global_var_names:
            global_var_names.append(name)


def write_var_tables(f_h: TextIO, f_c: TextIO, global_var_names: List[str],
                     global_var_table: Dict[str, Dict[str, int]]) -> None:
    """
    Write a table for each command, listing the slot number of each output variable, or -1 for variables which the
    command does not have. The variables are listed in alphabetical order, and their positions in this order are
    given #defines of the form PARSE_INDEX_<variable name>.

    :param f_h:
        The C header file to write to
    :param f_c:
        The C source file to write to
    :param global_var_names:
        List of the names of all the output variables used by any command
    :param global_var_table:
        Dictionary of the variable tables of all commands, indexed by <directive>_<set option>
    :return:
        None
    """

    global_var_names.sort()
    for index, name in enumerate(global_var_names):
        f_h.write("#define PARSE_INDEX_{} {:d}\n".format(sanitize(name), index))

    for key in sorted(global_var_table.keys()):
        var_table: Dict[str, int] = global_var_table[key]
        f_h.write("extern const int PARSE_TABLE_{}[];\n".format(key))
        f_c.write("const int PARSE_TABLE_{}[] = {{".format(key))
        f_c.write(",".join("{:d}".format(var_table[name]) if name in var_table else "-1"
                           for name in global_var_names))
        f_c.write("};\n")


def compile_command(line: str, f_h: TextIO, f_c: TextIO, include_keys: Dict[str, int],
                    global_var_names: List[str], global_var_table: Dict[str, Dict[str, int]]) -> None:
    """
    Compile the definition of a single command, from parser_data.dat, writing it to the output C files.

    :param line:
        The line of parser_data.dat which defines the command
    :param f_h:
        The C header file to write to
    :param f_c:
        The C source file to write to
    :param include_keys:
        Dictionary of the slot numbers of all the #defines written so far, indexed by name, used to check for
        conflicting definitions
    :param global_var_names:
        List of the names of all the output variables used by any command
    :param global_var_table:
        Dictionary of the variable tables of all commands, indexed by <directive>_<set option>
    :return:
        None
    """

    outline: str = ""
    directive: str = "undefined"
    set_option: str = ""

    # Lists [ ... ] of repeated items have their own space of slot numbers. When we enter a list, the slot count and
    # variable names of the enclosing level are pushed onto these stacks.
    stack: List[int] = []
    stack_var_names: List[Dict[str, int]] = []
    list_sizes: List[int] = []

    var_names: Dict[str, int] = STANDARD_VARIABLES.copy()  # Slot numbers of the variables at the current list level
    var_table: Final[Dict[str, int]] = STANDARD_VARIABLES.copy()  # Slot numbers of all variables in the command
    var_count: int = len(var_names)  # The slot number to give to the next variable

    for word in line.split():
        if word in GRAMMAR_CHARACTERS:
            # Grammar characters are passed straight through
            outline += "{} ".format(word)
            if word == "[":
                # Lists are placed on the stack, and the number of variables in each list counted
                stack.append(var_count)
                stack_var_names.append(var_names)
                var_count = 1  # Start counting from variable 1 inside a list, as first value is pointer to next item
                var_names = {}
            continue

        # Split the word into the form <match string>:<output variable name>
        parts: List[str] = word[1:].split(":")
        # If first character is a colon, it should match a literal :, not act as a separator
        parts[0] = word[0] + parts[0]

        # Match string has form plot@1, meaning 'p' is short for 'plot'
        sub_parts: List[str] = parts[0].split("@")
        if len(parts) < 2:
            parts.append("X")  # If output variable name is not specified, stick it in 'X'.
        if len(sub_parts) > 1:
            if sub_parts[1] == "n":
                sub_parts[1] = "-1"  # An auto-complete length of 'n' is stored as -1
            parts.append(sub_parts[1])
            parts[0] = sub_parts[0]
        else:
            parts.append("{:d}".format(len(parts[0])))  # If no auto-complete length is given, whole string must match

        # At this point, parts is [ match string , output variable , string to store (blank if equals match string) ,
        # auto-complete len ]
        if len(parts) == 3:
            parts.insert(2, "")
        assert len(parts) == 4, "Syntax error in word '{}'.".format(word)
        var_name: str = parts[1]

        # The end of a list ]:name stores the list in the variable <name>
        if word.startswith("]:"):
            parts = ["]", word[2:], "", "1"]
            list_sizes.append(var_count)
            var_count = stack.pop()
            for item_name, slot in var_names.items():
                stack_var_names[-1]["{}_{}".format(item_name, var_name)] = slot
            var_names = stack_var_names.pop()

        # Directive names are stored for use in #defines to convert variable names into output slot numbers
        if var_name == 'directive':
            directive = parts[0] if parts[2] == "" else parts[2]

        # Set options are also used in #defines
        if var_name == 'set_option':
            set_option = (parts[0] if parts[2] == "" else parts[2]) + "_"
            if set_option[0] == "%":
                set_option = ""

        # Allocate a slot number to each new variable
        if var_name not in var_names:
            var_names[var_name] = var_count
            var_table[var_name] = var_count
            if parts[0] == "%p":
                var_count += 2  # Position vectors require 2 or 3 slots
            elif parts[0] == "%P":
                var_count += 3
            else:
                var_count += 1
        elif parts[0] in ["%p", "%P"]:
            logging.warning("Danger in command {}: sharing position variable name with other variables of different "
                            "lengths".format(directive))

        parts.append("{:d}".format(var_names[var_name]))  # parts[4] = slot number
        initial: str
        if word.startswith("]:"):
            initial = ""
            parts.append("{:d}".format(list_sizes[-1]))  # parts[5] = number of slots needed for this list's variables
        else:
            initial = "@"
            parts.append("0")  # ... or zero otherwise
        outline += initial + "@".join(parts) + " "

    # First word on each statement definition line is the number of variables in the root slot space
    f_c.write("{:d} ".format(var_count))
    f_c.write("{}\\n\\\n".format(outline))
    process_var_table(var_table=var_table, directive=directive, set_option=set_option,
                      global_var_names=global_var_names, global_var_table=global_var_table)

    # Write #defines to convert variable names into slot numbers
    for name, slot in var_names.items():
        if '%' not in directive:
            key: str = "PARSE_{}_{}{}".format(directive, set_option, sanitize(name))
            if (key in include_keys) and (include_keys[key] != slot):
                logging.warning("Repetition of key {}".format(key))
            include_keys[key] = slot
            f_h.write("#define {} {:d}\n".format(key, slot))


def generate_command_list(input_filename: str, h_filename: str, c_filename: str) -> None:
    """
    Compile the grammar of all of Pyxplot's commands into C source files.

    :param input_filename:
        The filename of the grammar definition, parser_data.dat
    :param h_filename:
        The filename of the C header file to write
    :param c_filename:
        The filename of the C source file to write
    :return:
        None
    """

    include_keys: Final[Dict[str, int]] = {}
    global_var_names: Final[List[str]] = []
    global_var_table: Final[Dict[str, Dict[str, int]]] = {}

    with open(input_filename, "r") as f_in, open(h_filename, "w") as f_h, open(c_filename, "w") as f_c:
        f_h.write("""// This file autogenerated by parser_data.py

#ifndef _CMDLIST_H
#define _CMDLIST_H 1

extern const char ppl_cmdList[];
""")

        f_c.write("""// This file autogenerated by parser_data.py

const char ppl_cmdList[] = "\\
""")

        # Loop through command definitions, ignoring blank lines and comments
        line: str
        for line in f_in:
            line = line.strip()
            if (len(line) < 1) or line.startswith("#"):
                continue
            compile_command(line=line, f_h=f_h, f_c=f_c, include_keys=include_keys,
                            global_var_names=global_var_names, global_var_table=global_var_table)

        # Finish up
        f_c.write("""";\n""")
        write_var_tables(f_h=f_h, f_c=f_c, global_var_names=global_var_names, global_var_table=global_var_table)
        f_h.write("""\n\n#endif""")


# Do it right away if we're run as a script
if __name__ == "__main__":
    # Ensure we're running in the root of the Pyxplot source tree
    our_path: Final[str] = os.path.split(os.path.abspath(__file__))[0]
    os.chdir(os.path.join(our_path, ".."))

    # Configure logging
    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s:%(filename)s:%(message)s",
                        datefmt="%d/%m/%Y %H:%M:%S")

    generate_command_list(input_filename="buildScripts/parser_data.dat",
                          h_filename="src/parser/cmdList.h",
                          c_filename="src/parser/cmdList.c")
