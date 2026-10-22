# Shared mutation-test runner. A module script defines MUTATIONS as a list of
# (name, relative_path, [(old, new), ...]) and calls run(). Each mutation is
# applied alone to an isolated copy of the source tree under /tmp, the tree
# is rebuilt (Debug, examples off) and all tests are run. Every `old` pattern
# must occur exactly once, so a stale table fails loudly instead of testing
# nothing.
import pathlib
import shutil
import subprocess
import sys


def _run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True)


def _failed_tests(ctest_stdout):
    names = []
    for line in ctest_stdout.splitlines():
        if " - " in line and ("(Failed)" in line or "(Subprocess" in line or "(SEGFAULT)" in line
                              or "(Child aborted)" in line):
            names.append(line.split(" - ", 1)[1].split(" (")[0])
    return names


def run(repo_root, mutations, work_name):
    repo = pathlib.Path(repo_root)
    work = pathlib.Path("/tmp") / work_name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    # Everything the build needs. Keep in sync with the root CMakeLists.txt:
    # a missing directory makes configuration fail (seen after M4 added testkit/,
    # and again after M6 added bench/).
    for item in ["CMakeLists.txt", "cmake", "include", "src", "apps", "testkit", "tests", "bench"]:
        src = repo / item
        (shutil.copytree if src.is_dir() else shutil.copy)(src, work / item)
    configure = _run("cmake -S . -B b -G Ninja -DLOB_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Debug", work)
    if configure.returncode != 0:
        print(configure.stdout + configure.stderr)
        sys.exit(2)

    survivors = 0
    for name, rel, edits in mutations:
        path = work / rel
        original = path.read_text()
        mutated = original
        for old, new in edits:
            count = mutated.count(old)
            if count != 1:
                print(f"[ERROR] '{name}': pattern found {count} times: {old!r}")
                sys.exit(2)
            mutated = mutated.replace(old, new)
        path.write_text(mutated)
        build = _run("cmake --build b", work)
        if build.returncode != 0:
            print(f"[KILLED-BUILD] {name}")
        else:
            test = _run("ctest --test-dir b", work)
            failed = _failed_tests(test.stdout)
            if test.returncode == 0:
                survivors += 1
                print(f"[SURVIVED] {name}")
            else:
                shown = ", ".join(failed[:6]) + (" ..." if len(failed) > 6 else "")
                print(f"[KILLED] {name}: {len(failed)} failing: {shown}")
        path.write_text(original)

    build = _run("cmake --build b", work)
    test = _run("ctest --test-dir b", work)
    print(f"[BASELINE] unmutated tree: build rc={build.returncode} ctest rc={test.returncode}")
    print(f"survivors: {survivors}/{len(mutations)}")
