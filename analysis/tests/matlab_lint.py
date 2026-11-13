#!/usr/bin/env python3
"""
matlab_lint.py - static check that the .m files avoid Octave-only syntax.

The scripts are executed here in GNU Octave only; ThingSpeak runs them in
MATLAB. Octave accepts constructs MATLAB rejects, so this looks for the usual
ones in analysis/matlab/*.m (not in tests/octave_shims, which is Octave-only on
purpose): double-quoted strings, '#' comments, endif/endfor/..., '!' and '!=',
'+=' style operators, printf/puts, and functions defined in a script file.
Also checks that nothing that looks like a real API key is in the files.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = sorted(glob.glob(os.path.join(HERE, "..", "matlab", "*.m")))
SCRIPTS_WITHOUT_FUNCTIONS = ("ast_nightly.m", "vis_")      # pasted into ThingSpeak as scripts


def strip_strings_and_comments(line):
    """Remove single-quoted strings and the % comment so that the code can be pattern-matched."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c == "'" and (i == 0 or not (line[i - 1].isalnum() or line[i - 1] in "_)]}.'")):
            j = i + 1
            while j < n:
                if line[j] == "'":
                    if j + 1 < n and line[j + 1] == "'":
                        j += 2
                        continue
                    break
                j += 1
            out.append("''")
            i = j + 1
            continue
        if c == "%":
            break
        out.append(c)
        i += 1
    return "".join(out)


RULES = [
    (r'"', "double-quoted text (a string object in MATLAB, char in Octave)"),
    (r"#", "'#' comment"),
    (r"\b(endif|endfor|endwhile|endfunction|end_try_catch|endswitch|end_unwind_protect|unwind_protect)\b", "Octave-only block keyword"),
    (r"!=|(?<![=~<>])!(?!=)", "'!' operator (use ~)"),
    (r"\+=|-=|\*=|/=|\+\+|--", "increment / compound assignment operator"),
    (r"\b(printf|puts|fputs|fdisp)\s*\(", "Octave-only output function"),
    (r"\b(datetime|timetable|string|duration)\s*\(", "datetime/string object (not in Octave 8)"),
]
ALLOWED = {("ast_nightly.m", "datetime/string object (not in Octave 8)"): 1}    # MATLAB-only branch for the clock


def main():
    problems = 0
    for path in FILES:
        name = os.path.basename(path)
        seen = {}
        text = open(path, encoding="utf-8").read()
        if not text.isascii():
            print("%s: non-ASCII characters" % name)
            problems += 1
        for no, line in enumerate(text.splitlines(), 1):
            code = strip_strings_and_comments(line)
            for pat, what in RULES:
                if re.search(pat, code):
                    seen[what] = seen.get(what, 0) + 1
                    if seen[what] > ALLOWED.get((name, what), 0):
                        print("%s:%d: %s: %s" % (name, no, what, line.strip()))
                        problems += 1
            if name.startswith(SCRIPTS_WITHOUT_FUNCTIONS) and re.match(r"\s*function\b", code):
                print("%s:%d: function definition in a ThingSpeak script" % (name, no))
                problems += 1
            if re.search(r"(KEY\s*=\s*')([A-Z0-9]{16})'", line):
                print("%s:%d: looks like a real API key" % (name, no))
                problems += 1
    print("MATLAB LINT %s: %d file(s), %d problem(s)" % ("FAILED" if problems else "PASSED", len(FILES), problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
