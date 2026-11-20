#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# test_pyxplot.py
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
Pyxplot's automated tests, run with Python's unittest framework:

    python3 tests/test_pyxplot.py                 # Run all tests
    python3 tests/test_pyxplot.py -k units        # Run tests whose names contain "units"
    python3 -m unittest discover -s tests         # Equivalent, using unittest's test discovery

Each test is a Pyxplot script tests/<suite>/<name>.ppl, which becomes the test method test_<name> of the TestCase
class Test<Suite>. The script is run in a fresh temporary directory, and passes if Pyxplot's output and exit status
are as expected:

* <name>.out holds the expected stdout. If it does not exist, the script must write nothing to stdout.
* <name>.err holds the expected stderr. If it does not exist, the script must write nothing to stderr.
* Pyxplot must exit with status 1 if <name>.err exists (since it reports errors), and with status 0 otherwise.

Pyxplot writes all error messages, including failed assertions, to stderr. So a script which checks its results
with the assert command passes so long as no assertion fails and no error occurs.

Before running, the following files are copied into the temporary directory:

* tests/lib/*.ppl, so that tests can load shared helper functions, e.g. load "testlib.ppl"
* Any files in tests/<suite>/data/, e.g. data files for the test scripts to read
* <name>.pyxplotrc, if it exists, which is copied to .pyxplotrc so that Pyxplot reads it as a configuration file

Options for a test may be given in comment lines at the top of its script, in the form "# test: <option>=<value>":

* mode=script (the default) runs the test as "pyxplot <name>.ppl". Pyxplot stops at the first error.
* mode=stdin pipes the test into "pyxplot -". Pyxplot reports each error and carries on, which suits tests of error
  messages.
* status=<n> sets the expected exit status, overriding the default described above.
* timeout=<seconds> sets how long the test may run before it is killed (default 60).

The environment is made reproducible: HOME points to an empty directory (so the user's own ~/.pyxplotrc is not
read), the timezone is UTC, and the locale is C. Any occurrence of the path of the temporary directory in the output
is replaced by <TMPDIR>.

The following environment variables control the tests:

* PYXPLOT - the path of the Pyxplot binary to test (default: bin/pyxplot in the source tree)
* PYXPLOT_TEST_UPDATE=1 - overwrite the expected output of each test with its actual output, instead of checking it.
  Check the changes carefully with git diff before committing them.
* PYXPLOT_TEST_KEEP=1 - keep the temporary working directories of tests, for debugging
"""

import glob
import os
import shutil
import signal
import subprocess
import tempfile
import unittest

from typing import Callable, Dict, Final, List, NamedTuple, Tuple

# The directory which holds the tests
TESTS_DIR: Final[str] = os.path.split(os.path.abspath(__file__))[0]

# Directory of Pyxplot scripts which are copied into the working directory of every test
LIB_DIR: Final[str] = os.path.join(TESTS_DIR, "lib")

# The Pyxplot binary to test
PYXPLOT: Final[str] = os.path.abspath(os.environ.get("PYXPLOT", os.path.join(TESTS_DIR, "..", "bin", "pyxplot")))

# Default number of seconds a test may run before it is killed
DEFAULT_TIMEOUT: Final[float] = 60


class TestRun(NamedTuple):
    """
    The outcome of running a single test script.
    """
    stdout: str  # What the test wrote to stdout
    stderr: str  # What the test wrote to stderr
    status: int  # The exit status of Pyxplot
    work_dir: str  # The temporary directory the test ran in (deleted unless PYXPLOT_TEST_KEEP is set)


def read_options(script: str) -> Dict[str, str]:
    """
    Read the options for a test, given in comment lines of the form "# test: <option>=<value>" at the top of its
    script.

    :param script:
        The filename of the test script
    :return:
        Dictionary of options
    """

    options: Dict[str, str] = {"mode": "script", "timeout": str(DEFAULT_TIMEOUT)}
    with open(script) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("#"):
                break
            if line.startswith("# test:"):
                for item in line[len("# test:"):].split():
                    key, _, value = item.partition("=")
                    options[key] = value
    return options


def read_expected(filename: str) -> str:
    """
    Read a file of expected output, which may not exist, in which case no output is expected.

    :param filename:
        The filename of the file of expected output
    :return:
        The expected output
    """

    if not os.path.exists(filename):
        return ""
    with open(filename, encoding="utf-8", errors="replace") as f:
        return f.read()


def run_script(script: str, options: Dict[str, str]) -> TestRun:
    """
    Run a test script in a fresh temporary directory.

    :param script:
        The filename of the test script
    :param options:
        The options for the test, as returned by read_options()
    :return:
        The outcome of running the script
    """

    suite_dir: Final[str] = os.path.dirname(script)
    stem: Final[str] = os.path.splitext(os.path.basename(script))[0]
    keep: Final[bool] = bool(os.environ.get("PYXPLOT_TEST_KEEP"))

    tmp_dir: Final[str] = tempfile.mkdtemp(prefix="pyxplot_test_")
    work_dir: Final[str] = os.path.join(tmp_dir, "work")
    home_dir: Final[str] = os.path.join(tmp_dir, "home")
    os.mkdir(home_dir)

    # Populate the working directory
    data_dir: Final[str] = os.path.join(suite_dir, "data")
    if os.path.isdir(data_dir):
        shutil.copytree(data_dir, work_dir)
    else:
        os.mkdir(work_dir)
    for item in glob.glob(os.path.join(LIB_DIR, "*.ppl")):
        shutil.copy(item, work_dir)
    shutil.copy(script, work_dir)
    rc_file: Final[str] = os.path.join(suite_dir, stem + ".pyxplotrc")
    if os.path.exists(rc_file):
        shutil.copy(rc_file, os.path.join(work_dir, ".pyxplotrc"))

    env: Final[Dict[str, str]] = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": home_dir, "TZ": "UTC",
                                  "LANG": "C", "LC_ALL": "C", "TERM": "dumb"}

    # Run the test. Output is captured in files rather than pipes, since Pyxplot's helper processes inherit its
    # stdout and stderr, and if Pyxplot crashed, they could keep a pipe open indefinitely. Pyxplot is run in its own
    # process group, so that any helper processes left behind after a crash or timeout can be killed.
    script_name: Final[str] = os.path.basename(script)
    stdout_file: Final[str] = os.path.join(tmp_dir, "stdout")
    stderr_file: Final[str] = os.path.join(tmp_dir, "stderr")
    try:
        if options["mode"] == "stdin":
            command: List[str] = [PYXPLOT, "-"]
            stdin_file: str = os.path.join(work_dir, script_name)
        elif options["mode"] == "script":
            command = [PYXPLOT, script_name]
            stdin_file = os.devnull
        else:
            raise ValueError("Unknown test mode <{}>".format(options["mode"]))

        with open(stdin_file, "rb") as f_in, open(stdout_file, "wb") as f_out, open(stderr_file, "wb") as f_err:
            proc = subprocess.Popen(command, stdin=f_in, stdout=f_out, stderr=f_err, cwd=work_dir, env=env,
                                    start_new_session=True)
            try:
                status: int = proc.wait(timeout=float(options["timeout"]))
            except subprocess.TimeoutExpired:
                kill_process_group(proc.pid)
                proc.wait()
                raise
            if status < 0:
                kill_process_group(proc.pid)  # Pyxplot crashed; clean up its helper processes

        with open(stdout_file, "rb") as f:
            stdout: str = f.read().decode("utf-8", errors="replace").replace(tmp_dir, "<TMPDIR>")
        with open(stderr_file, "rb") as f:
            stderr: str = f.read().decode("utf-8", errors="replace").replace(tmp_dir, "<TMPDIR>")
    finally:
        if not keep:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    return TestRun(stdout=stdout, stderr=stderr, status=status, work_dir=work_dir)


def kill_process_group(pid: int) -> None:
    """
    Kill all of the processes in a process group, ignoring the error if they have all exited already.

    :param pid:
        The ID of the process group, which is the process ID of its leader
    :return:
        None
    """

    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def update_expected(script: str, run: TestRun) -> None:
    """
    Overwrite the files of expected output for a test with its actual output. Files for streams with no output are
    removed.

    :param script:
        The filename of the test script
    :param run:
        The outcome of running the test
    :return:
        None
    """

    stem: Final[str] = os.path.splitext(script)[0]
    for extension, actual in ((".out", run.stdout), (".err", run.stderr)):
        filename: str = stem + extension
        if actual:
            with open(filename, "w", encoding="utf-8") as f:
                f.write(actual)
        elif os.path.exists(filename):
            os.unlink(filename)


class PyxplotScriptTest(unittest.TestCase):
    """
    Base class for the TestCase classes which run the test scripts in each suite.
    """

    maxDiff = None  # Always show the full differences between expected and actual output

    def check_script(self, script: str) -> None:
        """
        Run a test script, and check that its output and exit status are as expected.

        :param script:
            The filename of the test script
        :return:
            None
        """

        if not os.access(PYXPLOT, os.X_OK):
            self.fail("Cannot find the Pyxplot binary <{}>. Run make first.".format(PYXPLOT))

        options: Final[Dict[str, str]] = read_options(script)
        try:
            run: Final[TestRun] = run_script(script=script, options=options)
        except subprocess.TimeoutExpired:
            self.fail("Test timed out after {} seconds.".format(options["timeout"]))

        if os.environ.get("PYXPLOT_TEST_UPDATE"):
            update_expected(script=script, run=run)
            return

        stem: Final[str] = os.path.splitext(script)[0]
        expected_stdout: Final[str] = read_expected(stem + ".out")
        expected_stderr: Final[str] = read_expected(stem + ".err")
        expected_status: Final[int] = int(options.get("status", "1" if os.path.exists(stem + ".err") else "0"))
        where: Final[str] = (" (working directory kept in <{}>)".format(run.work_dir)
                             if os.environ.get("PYXPLOT_TEST_KEEP") else "")

        if run.status < 0:
            self.fail("Pyxplot crashed with signal {:d} ({}){}.\nstderr:\n{}".format(
                -run.status, signal.Signals(-run.status).name, where, run.stderr))
        self.assertEqual(expected_stderr, run.stderr, "Unexpected output to stderr" + where)
        self.assertEqual(expected_stdout, run.stdout, "Unexpected output to stdout" + where)
        self.assertEqual(expected_status, run.status, "Unexpected exit status" + where)


def make_test_method(script: str) -> Callable[[PyxplotScriptTest], None]:
    """
    Make a test method which runs a single test script.

    :param script:
        The filename of the test script
    :return:
        The test method
    """

    def test_method(self: PyxplotScriptTest) -> None:
        self.check_script(script)

    test_method.__doc__ = "{}/{}".format(os.path.basename(os.path.dirname(script)), os.path.basename(script))
    return test_method


def make_test_classes() -> List[Tuple[str, type]]:
    """
    Make a TestCase class for each suite of test scripts, with a test method for each script.

    :return:
        List of (class name, class) for each suite
    """

    classes: List[Tuple[str, type]] = []
    for suite_dir in sorted(glob.glob(os.path.join(TESTS_DIR, "*"))):
        if (not os.path.isdir(suite_dir)) or (suite_dir == LIB_DIR) or suite_dir.endswith("__pycache__"):
            continue
        suite: str = os.path.basename(suite_dir)
        methods: Dict[str, Callable] = {}
        for script in sorted(glob.glob(os.path.join(suite_dir, "*.ppl"))):
            stem: str = os.path.splitext(os.path.basename(script))[0]
            methods["test_{}".format(stem)] = make_test_method(script)
        if methods:
            class_name: str = "Test" + "".join(word.capitalize() for word in suite.split("_"))
            classes.append((class_name, type(class_name, (PyxplotScriptTest,), methods)))
    return classes


# Create a TestCase class for each suite of tests, so that unittest can find them
for _name, _cls in make_test_classes():
    globals()[_name] = _cls
del _name, _cls  # Otherwise unittest would find the last class a second time, under the name _cls

# Do it right away if we're run as a script
if __name__ == "__main__":
    unittest.main()
