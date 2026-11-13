#!/usr/bin/env python3
"""Create a new program that builds on this buddhabox-tui checkout.

Usage:
    python3 tools/new_project.py ../my_synth
    python3 tools/new_project.py ../bells --example singing_bowls

What it writes into the new folder:
    main.cpp     a copy of one of the examples (default: sine) to start
                 from
    Makefile     builds main.cpp with this checkout's library into a
                 program named after the folder
    .gitignore   ignores that program and its recordings

HOW THE LINK WORKS: the Makefile's BBTUI line points at this checkout.
Its headers and .cpp files are compiled straight from there, so changes
to the library show up in every project on the next build. The path is
written relative to the new project when the two share a parent folder
(for example, both under ~/git), so they can move together without
breaking it; otherwise it is absolute.

Adapted from buddhabox-framework's tools/new_project.py, which writes a
platformio.ini instead. Uses only the Python standard library.
"""

import argparse
import os
import shutil
import sys

# This checkout's root: this script lives in <root>/tools/.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXAMPLES = os.path.join(ROOT, "examples")

# Doubled braces are literal braces once .format() has run; make needs
# its own $(...) left alone, and those contain no braces.
MAKEFILE = """\
# {name}: built on buddhabox-tui.
#
#   make           build ./{name}
#   ./{name}       run it (Ctrl-C to quit)
#   make clean     remove it

# The buddhabox-tui checkout this project builds on.
BBTUI ?= {bbtui}

CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -I$(BBTUI)/src

ifeq ($(shell uname),Darwin)
  LDLIBS = -framework AudioToolbox -framework CoreMIDI \\
           -framework CoreFoundation
else
  LDLIBS = -lasound -lpthread
endif

LIB = $(wildcard $(BBTUI)/src/bbtui/*.cpp)
HEADERS = $(wildcard $(BBTUI)/src/*/*.h $(BBTUI)/src/*/*/*.h)

{name}: main.cpp $(LIB) $(HEADERS)
\t$(CXX) $(CXXFLAGS) main.cpp $(LIB) -o $@ $(LDLIBS)

clean:
\trm -f {name}

.PHONY: clean
"""


def main():
    examples = sorted(os.listdir(EXAMPLES))
    parser = argparse.ArgumentParser(
        description="Create a new program on buddhabox-tui.")
    parser.add_argument("path", help="folder to create, e.g. ../my_synth")
    parser.add_argument("--example", default="sine", choices=examples,
                        help="example to copy as main.cpp")
    args = parser.parse_args()

    # 1. Refuse to write over anything that is already there.
    target = os.path.abspath(args.path)
    if os.path.exists(target) and os.listdir(target):
        sys.exit("error: %s exists and is not empty" % target)

    # 2. The program's name, from the folder: letters, digits, - and _
    #    only, so it is a safe file name and make target.
    name = "".join(c if c.isalnum() or c in "-_" else "_"
                   for c in os.path.basename(target)) or "program"

    # 3. Write the three files. Relative path to this checkout when the
    #    two folders share a parent below /; otherwise a run of
    #    ../../.. up to the root would be fragile, so absolute.
    os.makedirs(target, exist_ok=True)
    if os.path.commonpath([ROOT, target]) == os.sep:
        bbtui = ROOT
    else:
        bbtui = os.path.relpath(ROOT, target)
    with open(os.path.join(target, "Makefile"), "w") as f:
        f.write(MAKEFILE.format(name=name, bbtui=bbtui))
    with open(os.path.join(target, ".gitignore"), "w") as f:
        f.write("%s\n*.wav\n.DS_Store\n" % name)
    shutil.copy(os.path.join(EXAMPLES, args.example, "main.cpp"),
                os.path.join(target, "main.cpp"))

    print("created %s (from examples/%s)" % (target, args.example))
    print("next: cd %s && make && ./%s" % (args.path, name))


if __name__ == "__main__":
    main()
