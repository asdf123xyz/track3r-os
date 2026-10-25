#!/usr/bin/env python3
"""PreToolUse(Bash, Read) guard: C++ symbols are found with the LSP tool, C/C++ files are edited with Edit/Write,
long files are read by range.

Rule 1, advisory (PostToolUse Bash, `--post`): a recursive grep/rg/git grep for a C++ identifier over the source
tree runs, and at most one reminder every 15 minutes per worktree points at build-map/ and the LSP tool. Searches
of docs, build-map/, strings with spaces, ALL_CAPS names, filters after a pipe and commands with `O3_NO_LSP=1` in
front get no reminder. Maintainer, 2026-10-24: as a refusal it cost about 100 blocked-then-rerun calls a day while
the shared clangd index (one server for every lane) rarely answered.

Rule 2, a Bash command that rewrites a .c/.cc/.cpp/.h/.hpp file of the tree (sed -i, perl -i, a redirect, tee,
a python/perl/ruby/node write that names a C/C++ path, glob or extension anywhere in its text, heredoc or script
file, cp/mv/install/dd/ed/ex over a source) is refused: an Edit or Write runs the PostToolUse syntax check, a shell
edit does not. A sed -i is judged by its target files, not by its script text. For a
mechanical edit over many files, `O3_BULK_EDIT=1` in front passes; then run
`python3 -I .claude/hooks/syntax_check.py <files>` on the files it touched.

Rule 3 (PreToolUse Read and Bash), a whole read of a text file over 400 lines is refused: a Read with no offset
and no limit, or a `cat` whose output goes to no pipe. Find the function with the LSP and read its range; an
explicit `limit` (up to the whole file) passes, as does `O3_WHOLE_READ=1` in front of a cat.

Maintainer, 2026-10-22: the OS-QAFIX engineers made no LSP call and every edit went through Bash; whole-file
reads are the next largest use of context. `code_nav_guard.py --selftest` checks the rules.
"""
import hashlib
import json
import os
import re
import shlex
import sys
import tempfile
import time

MAX_WHOLE_LINES = 400

CXX_EXT = r"\.(?:c|cc|cpp|h|hh|hpp)"
CXX_PATH = re.compile(r"[\w./-]*" + CXX_EXT + r"\b")
SOURCE_DIRS = ("app", "audio", "core", "eve", "formats", "hal", "midi", "model", "module", "modules",
               "platform", "seq", "sim", "tests", "ui", "usb")
TEXT_PATHS = re.compile(r"(^|/)(docs|build-map|build[^/]*|third_party)(/|$)|\.(md|txt|json|cmake|py|sh|ld|csv)$"
                        r"|CMakeLists")
IDENT = re.compile(r"^~?[A-Za-z_][A-Za-z0-9_]*(::~?[A-Za-z_][A-Za-z0-9_]*)*$")


def segments(command):
    """(segment, follows_pipe) for each simple command; separators inside quotes do not split."""
    out, cur, quote, piped, i = [], [], None, False, 0
    while i < len(command):
        c = command[i]
        if quote:
            quote = None if c == quote else quote
        elif c in "'\"":
            quote = c
        elif c in ";\n|" or command.startswith("&&", i):
            double = command.startswith(("&&", "||"), i)
            out.append(("".join(cur).strip(), piped))
            piped = c == "|" and not double
            cur, i = [], i + (2 if double else 1)
            continue
        cur.append(c)
        i += 1
    out.append(("".join(cur).strip(), piped))
    return [(s, p) for s, p in out if s]


def tokens(segment):
    try:
        toks = shlex.split(segment)
    except ValueError:
        toks = segment.split()
    while toks and (re.match(r"^\w+=", toks[0]) or toks[0] in ("nice", "time", "command", "xargs")):
        toks = toks[1:]
        if toks and re.match(r"^-n?\d*$", toks[0]):  # nice -n 10, xargs -0
            toks = toks[1:]
            if toks and toks[0].isdigit():
                toks = toks[1:]
    return toks


def is_symbol(pattern):
    alternatives = [a for a in re.split(r"\\\||\|", pattern) if a]
    if not alternatives:
        return False
    for alt in alternatives:
        alt = re.sub(r"^\^|\$$|\\b|\\<|\\>|^\(|\)$", "", alt)
        if not IDENT.match(alt) or len(alt) < 4 or alt.upper() == alt:
            return False
        if not ("_" in alt.strip("_") or "::" in alt or re.search(r"[a-z][A-Z]", alt)):
            return False
    return True


def symbol_search(toks, piped):
    if not toks or piped:
        return None
    if toks[0] == "git" and len(toks) > 1 and toks[1] == "grep":
        tool, args, recursive = "git grep", toks[2:], True
    elif toks[0] in ("grep", "egrep"):
        tool, args = toks[0], toks[1:]
        recursive = any(a in ("--recursive", "--dereference-recursive") or
                        (a.startswith("-") and not a.startswith("--") and re.search("[rR]", a)) for a in args)
    elif toks[0] == "rg":
        tool, args, recursive = "rg", toks[1:], True
    else:
        return None
    pattern, paths, filters, skip = None, [], [], False
    takes_value = {"-e", "-f", "-m", "-A", "-B", "-C", "-g", "--glob", "-t", "--type", "--include", "--exclude",
                   "--exclude-dir", "--max-count"}
    filter_flags = ("-g", "--glob", "-t", "--type", "--include")
    for i, a in enumerate(args):
        if skip:
            skip = False
            if args[i - 1] == "-e" and pattern is None:
                pattern = a
            elif args[i - 1] in filter_flags:
                filters.append(a)
            continue
        if a in takes_value:
            skip = True
        elif a.startswith(tuple(f + "=" for f in filter_flags if f.startswith("--"))):
            filters.append(a.split("=", 1)[1])
        elif a.startswith("-"):
            continue
        elif pattern is None:
            pattern = a
        else:
            paths.append(a)
    if pattern is None or not is_symbol(pattern):
        return None
    if filters and not any(re.search(CXX_EXT + r"\b", f) or f in ("c", "cpp", "h") for f in filters):
        return None  # limited to CMake, scripts or docs
    if paths and all(TEXT_PATHS.search(p) for p in paths):
        return None
    if not recursive and not any("*" in p for p in paths) and len(paths) < 3:
        return None  # one or two named files: a read, not a search
    if paths and not any(p.split("/")[0] in SOURCE_DIRS or p in (".", "./") or re.search(CXX_EXT + r"$", p)
                         or p.startswith("/") for p in paths):
        return None
    return f"{tool} for the C++ symbol `{pattern}`"


CXX_TOKEN = re.compile(r"[\w./*?\[\]{},~+@$-]*(?:\.(?:c|cc|cpp|h|hh|hpp)|\.\[ch\]p*|\.\{[\w,]*\})")
# A quoted literal that is a whole C/C++ path, glob or extension ('app/x.cpp', '**/*.cpp', '.h', f'{n}.cpp');
# a message such as 'see editor.cpp' is not (the name does not start the literal).
CXX_LITERAL = re.compile(r"""(?<=['"])(?:[\w./*?\[\]{}$~+@-]*(?:\.(?:c|cc|cpp|h|hh|hpp)|\.\[ch\]p*|\.\{[\w,]*\})"""
                         r"""|cpp|hpp)(?=['"])""")
INTERP = re.compile(r"^(?:python[0-9.]*|perl|ruby|node(?:js)?)$")
SCRIPT_EXT = (".py", ".pl", ".rb", ".js", ".mjs", ".cjs")
SHELLS = ("sh", "bash", "zsh", "dash")
# Write markers of an interpreter's text: python/ruby/node/perl file writes, in-place edits, renames.
WRITE_MARK = re.compile(
    r"write_text\(|write_bytes\(|\.write\(|\.writelines\(|\.truncate\(|\.rename\(|\.unlink\(|os\.(?:replace|rename|remove|unlink)"
    r"|shutil|inplace|writeFile|appendFile|createWriteStream|O_WRONLY|O_RDWR"
    r"""|open\(\s*['"][rwabxt+]*[wax+][rwabxt+]*['"]\s*[,)]"""          # Path.open('w')
    r"""|open\([^)]*,\s*(?:mode\s*=\s*)?['"][rwabxt]*[wax+][rwabxt+]*['"]"""  # open(p, 'w'|'a'|'r+')
    r"""|open\s*\(?[^;\n]*['"]\s*\+?>|['"]-[a-zA-Z]*i[a-zA-Z.]*['"]""")      # perl open(F, ">x"), ['sed','-i']
TMP_OPEN = re.compile(r"""open\(\s*['"]/tmp[^)]*\)(?:\.write\()?""")  # a write into /tmp is not an edit of the tree


def tree_path(p):
    return not p.startswith("/tmp") and "build" not in p.split("/")[0]


def cxx_arg(a):
    return bool(CXX_TOKEN.fullmatch(a)) and tree_path(a)


def writes_in_place(cmd, args):
    if cmd in ("sed", "perl", "ruby"):
        return any(re.match(r"^-[a-zA-Z]*i", a) or a.startswith("--in-place") for a in args)
    return cmd == "awk" and "inplace" in " ".join(args)


def script_text(toks, cwd):
    """The text of the script file an interpreter segment runs (python3 /tmp/fix.py), else ''."""
    for a in toks[1:]:
        if a.startswith("-") and a not in ("-",):
            continue
        if a.endswith(SCRIPT_EXT):
            full = os.path.join(cwd or os.getcwd(), os.path.expanduser(a))
            if not full.startswith("/tmp/"):
                return ""  # tracked tools/ scripts write their own outputs; only throwaway scripts are judged
            try:
                with open(full, errors="replace") as f:
                    return f.read(1 << 19)
            except OSError:
                return ""
        break
    return ""


def interp_edit(command, segs, cwd):
    """A python/perl/ruby/node command that names a C/C++ path and writes: judged on the whole command text
    (heredoc, -c string, the script file it runs), whatever the variable or loop that carries the path."""
    texts, args = [command], []
    for seg, _ in segs:
        toks = tokens(seg)
        if toks and INTERP.match(os.path.basename(toks[0])):
            texts.append(script_text(toks, cwd))
            args += toks[1:]
    text = "\n".join(texts).replace('\\"', '"').replace("\\'", "'")
    refs = [m for m in CXX_LITERAL.findall(text) if tree_path(m) or m in ("cpp", "hpp")]
    refs += [a for a in args if cxx_arg(a)]
    if refs and WRITE_MARK.search(TMP_OPEN.sub("", text)):
        return f"a script write (python/perl/ruby/node) that names {refs[0]}"
    return None


def shell_edit(segment, toks, fed=False):
    """fed: the segment follows a pipe whose left side names C/C++ files (xargs sed -i)."""
    if not toks:
        return None
    cmd, args = os.path.basename(toks[0]), toks[1:]
    files = [a for a in args if cxx_arg(a)]
    if cmd in SHELLS and "-c" in args[:-1]:
        return command_edit(args[args.index("-c") + 1], None)
    if writes_in_place(cmd, args) and (files or (fed and "xargs" in segment)):
        return f"{cmd} -i on {files[0] if files else 'the files piped in'}"
    for flag in ("-exec", "-execdir"):
        if cmd == "find" and flag in args and files:  # find -name '*.cpp' -exec sed -i ... {} +
            sub = args[args.index(flag) + 1:]
            if sub and writes_in_place(os.path.basename(sub[0]), sub[1:]):
                return f"find {flag} {sub[0]} -i on {files[0]}"
    if cmd in ("tee", "sponge", "truncate", "ed", "ex", "vi", "vim", "nvim") and files:
        return f"{cmd} on {files[0]}"
    operands = [a for a in args if not a.startswith("-")]
    anywhere = [a for a in operands if CXX_TOKEN.fullmatch(a)]
    if cmd in ("cp", "install", "rsync", "ln", "mv") and anywhere and operands:
        dest = operands[-1]
        if cmd == "mv" and files:
            return f"mv of {files[0]}"
        if cxx_arg(dest) or (tree_path(dest) and any(not tree_path(a) for a in anywhere)):
            return f"{cmd} of {anywhere[0]} over a source"
    if cmd == "dd":
        out = [a[3:] for a in args if a.startswith("of=") and cxx_arg(a[3:])]
        if out:
            return f"dd into {out[0]}"
    redirect = re.search(r"(?<![0-9&])>>?\s*['\"]?([\w./-]*" + CXX_EXT + r")\b", segment)
    if redirect and tree_path(redirect.group(1)):
        return f"a redirect into {redirect.group(1)}"
    return None


def command_edit(command, cwd):
    """The reason a Bash command rewrites a C/C++ source of the tree, else None."""
    segs = segments(command)
    if any(toks and INTERP.match(os.path.basename(toks[0])) for toks in (tokens(s) for s, _ in segs)):
        what = interp_edit(command, segs, cwd)
        if what:
            return what
    for i, (seg, piped) in enumerate(segs):
        fed = piped and i > 0 and any(CXX_TOKEN.fullmatch(t) for t in tokens(segs[i - 1][0]))
        what = shell_edit(seg, tokens(seg), fed)
        if what:
            return what
    return None


def long_text(path, cwd=None):
    """The line count of a text file over MAX_WHOLE_LINES, else None."""
    full = os.path.join(cwd or os.getcwd(), os.path.expanduser(path))
    try:
        if not os.path.isfile(full) or os.path.getsize(full) > 64 << 20:
            return None
        with open(full, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if b"\0" in data[:4096]:
        return None  # binary: images and PDFs are not this rule's business
    lines = data.count(b"\n")
    return lines if lines > MAX_WHOLE_LINES else None


def whole_read_reason(what, lines):
    return (f"BLOCKED by project guard: {what} reads all {lines} lines. Find the function with the LSP "
            "(goToDefinition, findReferences, hover) or build-map/, then Read its range with offset/limit; "
            "pass an explicit limit if the whole file is truly needed.")


def whole_cat(command, cwd):
    if "O3_WHOLE_READ=1" in command:
        return None
    segs = segments(command)
    for i, (seg, _) in enumerate(segs):
        toks = tokens(seg)
        if not toks or toks[0] != "cat" or "<<" in seg or ">" in seg:
            continue
        if i + 1 < len(segs) and segs[i + 1][1]:
            continue  # cat file | grep ...: the pipe filters it
        for arg in toks[1:]:
            lines = None if arg.startswith("-") else long_text(arg, cwd)
            if lines:
                return whole_read_reason(f"cat {arg}", lines)
    return None


def check_read(tool_input, cwd=None):
    if tool_input.get("offset") is not None or tool_input.get("limit") is not None:
        return None
    path = tool_input.get("file_path") or ""
    lines = long_text(path, cwd)
    return whole_read_reason(f"Read of {os.path.basename(path)}", lines) if lines else None


def check(command, cwd=None):
    """None, or the reason the command is refused."""
    reason = whole_cat(command, cwd)
    if reason:
        return reason
    if "O3_BULK_EDIT=1" not in command:
        what = command_edit(command, cwd)
        if what:
            return (f"BLOCKED by project guard: {what}. Edit C/C++ files with the Edit or Write tool, so the "
                    "PostToolUse syntax check runs on them. For a mechanical edit over many files, put "
                    "O3_BULK_EDIT=1 in front and then run `python3 -I .claude/hooks/syntax_check.py <files>` "
                    "on every file it touched.")
    return None


def symbol_hint(command):
    """None, or the reminder for a symbol search (rule 1, advisory)."""
    if "O3_NO_LSP=1" in command:
        return None
    for seg, piped in segments(command):
        what = symbol_search(tokens(seg), piped)
        if what:
            return (f"Reminder (project guard, rule 1): {what}. Next time, build-map/ (keys.md, screens.md, "
                    "features.md) or the LSP tool (goToDefinition, findReferences from a file:line) usually answer "
                    "with less output. No action needed for this command.")
    return None


def judge(command, cwd=None):
    """What the guard does with a command: the refusal, else the reminder, else None (selftest)."""
    return check(command, cwd) or symbol_hint(command)


HINT_EVERY_S = 15 * 60


def post(payload):
    """PostToolUse Bash: the reminder of rule 1, at most once every HINT_EVERY_S per worktree."""
    hint = symbol_hint((payload.get("tool_input") or {}).get("command") or "")
    if not hint:
        return 0
    key = hashlib.sha1((payload.get("cwd") or "").encode()).hexdigest()[:12]
    mark = os.path.join(tempfile.gettempdir(), f"o3-navhint-{key}")
    try:
        if time.time() - os.path.getmtime(mark) < HINT_EVERY_S:
            return 0
    except OSError:
        pass
    try:
        with open(mark, "w"):
            pass
    except OSError:
        return 0
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": hint}}))
    return 0


SELFTEST = [
    ("grep -rn begin_block audio/", True),
    ("grep -rn 'SampleBank::poll' .", True),
    ("rg -n preview_bind", True),
    ("git grep -n startProjectLoad", True),
    ("grep -rn 'begin_block\\|end_block' audio/src", True),
    ("O3_NO_LSP=1 grep -rn begin_block audio/", False),
    ("grep -rn 'PROJECT FAILED' app/", False),
    ("grep -rn O3OS_CI_JOBS tools/", False),
    ("grep -n begin_block build-map/keys.md", False),
    ("grep -rn begin_block docs/", False),
    ("git log --oneline | grep fix_slice", False),
    ("grep -c begin_block audio/src/engine.cpp", False),
    ("grep -n begin_block audio/src/engine.cpp", False),
    ("grep -rn sample docs/ app/", False),
    ("sed -n 10,40p audio/src/engine.cpp", False),
    ("sed -i 's/a/b/' audio/src/engine.cpp", True),
    ("O3_BULK_EDIT=1 sed -i 's/a/b/' audio/src/engine.cpp", False),
    ("perl -pi -e 's/a/b/' app/src/app.cpp", True),
    ("cat > app/src/new_file.cpp <<'EOF'\nint x;\nEOF", True),
    ("echo x 2>/dev/null; cat audio/src/engine.cpp > /tmp/x.cpp", False),
    ("python3 - <<'EOF'\nimport pathlib\np=pathlib.Path('app/src/app.cpp')\np.write_text('x')\nEOF", True),
    ("python3 - <<'EOF'\nimport pathlib\nprint(pathlib.Path('app/src/app.cpp').read_text()[:10])\nEOF", False),
    ("python3 - <<'EOF'\nimport pathlib\np=pathlib.Path('docs/a.md')\np.write_text('see editor.cpp')\nEOF\ngit commit -m 'x'",
     False),
    # Python/perl/ruby/node writes with the path in a variable, a loop, a glob or an f-string.
    ("python3 - <<'EOF'\nfrom pathlib import Path\nd=Path('tests')\nfor p in d.glob('*.cpp'):\n    p.write_text(p.read_text())\nEOF",
     True),
    ("python3 - <<'EOF'\nimport pathlib\np=pathlib.Path('tests')/'test_x.cpp'\ns=p.read_text()\np.write_text(s.replace('a','b'))\nEOF",
     True),
    ("python3 - <<'EOF'\nimport re\np='app/src/app.cpp'\ns=open(p).read()\ns=re.sub('a','b',s)\nopen(p,'w').write(s)\nEOF", True),
    ("python3 - <<'EOF'\np='app/src/app.cpp'\nwith open(p,'r+') as f:\n    f.truncate(0)\nEOF", True),
    ("python3 - <<'EOF'\nimport io\nwith io.open('app/src/app.cpp', mode='a') as f:\n    f.write('x')\nEOF", True),
    ("python3 - <<'EOF'\nimport os\nfor r,_,fs in os.walk('app'):\n    for n in fs:\n        if n.endswith(('.cpp','.h')):\n"
     "            open(os.path.join(r,n),'w').write('')\nEOF", True),
    ("python3 -c \"import pathlib; pathlib.Path('app/src/app.cpp').write_text('x')\"", True),
    ("python3 -c \"print(open('app/src/app.cpp').read()[:20])\"", False),
    ("python3 -c \"import pathlib; pathlib.Path('docs/n.md').write_text('see record_actions.cpp')\"", False),
    ("python3 -c \"import shutil; shutil.copy('/tmp/x', 'app/src/app.cpp')\"", True),
    ("python3 -c \"import os; os.replace('/tmp/x.cpp', 'app/src/app.cpp')\"", True),
    ("python3 tools/fix.py app/src/app.cpp && true", False),
    ("python3 tools/code_map.py", False),
    ("python3 -I tools/check_layers.py", False),
    ("python3 -I .claude/hooks/syntax_check.py app/src/app.cpp", False),
    ("python3 - <<'EOF'\nimport json\nopen('/tmp/out.json','w').write(json.dumps(open('app/src/app.cpp').read()))\nEOF",
     False),
    ("cat > /tmp/fix.py <<'EOF'\nfrom pathlib import Path\nPath('app/src/app.cpp').write_text('x')\nEOF\npython3 /tmp/fix.py",
     True),
    ("perl -i.bak -pe 's/a/b/' app/src/app.cpp", True),
    ("perl -ne 'print if /a/' app/src/app.cpp", False),
    ("ruby -pi -e '$_.upcase!' app/src/app.cpp", True),
    ("node -e \"require('fs').writeFileSync('app/src/app.cpp','x')\"", True),
    ("node -e \"console.log(require('fs').readFileSync('app/src/app.cpp','utf8').length)\"", False),
    ("ed -s app/src/app.cpp <<'EOF'\n,s/a/b/\nw\nq\nEOF", True),
    ("ex -sc '%s/a/b/|x' app/src/app.cpp", True),
    ("cp /tmp/new.cpp app/src/app.cpp", True),
    ("cp /tmp/new.cpp app/src/", True),
    ("mv /tmp/new.h app/src/app.h", True),
    ("mv app/src/old.cpp /tmp/old.cpp", True),
    ("cp app/src/app.cpp /tmp/backup.cpp", False),
    ("cp app/src/app.cpp /tmp/", False),
    ("dd if=/tmp/new.cpp of=app/src/app.cpp", True),
    ("dd if=app/src/app.cpp of=/tmp/x.bin", False),
    ("echo x | sponge app/src/app.cpp", True),
    ("bash -c \"sed -i 's/a/b/' app/src/app.cpp\"", True),
    ("find app -name '*.cpp' -exec sed -i 's/a/b/' {} +", True),
    ("git ls-files '*.cpp' | xargs sed -i 's/a/b/'", True),
    ("sed -i 's/a/b/' app/*.cpp", True),
    # Not edits of C/C++: git/grep/cat reads, notes whose text only mentions a source.
    ("git show HEAD:app/src/app.cpp | head -20", False),
    ("git diff -- app/src/app.cpp", False),
    ("cat app/src/app.cpp | grep -n foo", False),
    ("sed -i 's/old/see record_actions.cpp/' docs/notes.md", False),
    ("sed -i 's/old/see record_actions.cpp/' notes.txt", False),
    ("echo 'see record_actions.cpp' >> docs/notes.md", False),
    ("grep -rn update_goldens --include=CMakeLists.txt --include=*.py .", False),
    ("rg -n update_goldens -g '*.cmake'", False),
    ("grep -rn begin_block --include=*.cpp .", True),
    ("sed -i 's/a/b/' docs/STATUS.md", False),
    ("cmake --build build-os-host -j3 2>&1 | tail -5", False),
]


def selftest():
    bad = [(cmd, want) for cmd, want in SELFTEST if (judge(cmd) is not None) != want]
    bad += [(f"rule 1 must not refuse: {c}", False) for c in ("grep -rn begin_block audio/", "rg -n preview_bind")
            if check(c) is not None]
    with tempfile.TemporaryDirectory(dir="/tmp") as d:
        with open(os.path.join(d, "big.cpp"), "w") as f:
            f.write("int x;\n" * (MAX_WHOLE_LINES + 1))
        with open(os.path.join(d, "small.cpp"), "w") as f:
            f.write("int x;\n" * 10)
        reads = [
            ({"file_path": f"{d}/big.cpp"}, True),
            ({"file_path": f"{d}/big.cpp", "offset": 100, "limit": 50}, False),
            ({"file_path": f"{d}/big.cpp", "limit": 2000}, False),
            ({"file_path": f"{d}/small.cpp"}, False),
            ({"file_path": f"{d}/missing.cpp"}, False),
        ]
        bad += [(f"Read {t}", want) for t, want in reads if (check_read(t) is not None) != want]
        cats = [
            ("cat big.cpp", True),
            ("cat small.cpp", False),
            ("cat big.cpp | grep x", False),
            ("O3_WHOLE_READ=1 cat big.cpp", False),
            ("cat > big2.cpp <<'EOF'\nx\nEOF", False),
        ]
        bad += [(cmd, want) for cmd, want in cats if (whole_cat(cmd, d) is not None) != want]
        with open(os.path.join(d, "fix.py"), "w") as f:
            f.write("import pathlib\npathlib.Path('app/a.cpp').write_text('x')\n")
        with open(os.path.join(d, "look.py"), "w") as f:
            f.write("import pathlib\nprint(pathlib.Path('app/a.cpp').read_text())\n")
        scripts = [("python3 fix.py", True), ("python3 look.py", False), ("python3 missing.py", False),
                   ("python3 tools/code_map.py", False)]
        bad += [(f"script: {c}", w) for c, w in scripts if (check(c, d) is not None) != w]
    for cmd, want in bad:
        print(f"FAIL want {'block' if want else 'pass'}: {cmd!r}")
    total = len(SELFTEST) + 14
    print(f"{total - len(bad)}/{total} ok")
    return 1 if bad else 0


def main() -> int:
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    if not isinstance(payload, dict):
        return 0
    if sys.argv[1:] == ["--post"]:
        return post(payload)
    tool_input = payload.get("tool_input") or {}
    cwd = payload.get("cwd")
    if payload.get("tool_name") == "Read":
        reason = check_read(tool_input, cwd)
    else:
        reason = check(tool_input.get("command") or "", cwd)
    if reason:
        print(reason, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
