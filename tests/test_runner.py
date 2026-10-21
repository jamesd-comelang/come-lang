#!/usr/bin/env python3
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

NC = "\x1b[0m"
RED = "\x1b[1;38;2;255;255;255;48;2;200;0;0m"
GREEN = "\x1b[1;38;2;255;255;255;48;2;0;150;0m"


def is_windows():
    return os.name == "nt"


def exe_suffix():
    return ".exe" if is_windows() else ""


def rel(root, path):
    try:
        return str(Path(path).resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path)


def run(cmd, cwd, label=None):
    proc = subprocess.run(
        [str(c) for c in cmd],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    if proc.stdout:
        for line in proc.stdout.splitlines():
            if line.strip():
                print(f"    {line}")
    if proc.returncode != 0 and label:
        print(f"    [{RED}FAIL{NC}] {label}")
    return proc.returncode


def compiler_path(root):
    compiler = root / "build" / f"come{exe_suffix()}"
    if not compiler.exists() and not is_windows():
        compiler = root / "build" / "come"
    return compiler


def build_artifacts_ready(root):
    required = [
        compiler_path(root),
        root / "build" / "std.o",
        root / "build" / "string.o",
        root / "build" / "array.o",
        root / "build" / "map.o",
        root / "build" / "talloc.o",
    ]
    if not is_windows():
        required.append(root / "build" / "talloc_lib.o")
    return all(path.exists() for path in required)


def build_include_flags():
    return [
        "-Wall",
        "-g",
        "-D__STDC_WANT_LIB_EXT1__=1",
        "-Isrc/include",
        "-Isrc/core/include",
        "-Isrc/external/talloc/lib/talloc",
        "-Isrc/external/talloc/lib/replace",
    ]


def compile_object(root, gcc, flags, source, output):
    return run([gcc, *flags, "-c", root / source, "-o", root / output], root, f"Compile failed: {source}") == 0


def combine_objects(root, output, inputs):
    ld = shutil.which("ld")
    if ld:
        cmd = [ld, "-r", *[root / obj for obj in inputs], "-o", root / output]
    else:
        gcc = shutil.which("gcc")
        if not gcc:
            print(f"    [{RED}FAIL{NC}] ld/gcc not found")
            return False
        cmd = [gcc, "-r", *[root / obj for obj in inputs], "-o", root / output]
    return run(cmd, root, f"Relocatable link failed: {output}") == 0


def rebuild_project(root):
    gcc = shutil.which("gcc")
    if not gcc:
        print(f"    [{RED}FAIL{NC}] gcc not found")
        return False

    build_dir = root / "build"
    build_dir.mkdir(parents=True, exist_ok=True)
    flags = build_include_flags()

    print("Rebuilding compiler/runtime from current sources...")
    compiler_sources = [
        ("src/core/come_compiler.c", "build/come_compiler.o"),
        ("src/core/codegen.c", "build/codegen.o"),
        ("src/core/lexer.c", "build/lexer.o"),
        ("src/core/parser.c", "build/parser.o"),
        ("src/core/utils.c", "build/utils.o"),
        ("src/array/array.c", "build/array.o"),
        ("src/map/map.c", "build/map.o"),
        ("src/mem/talloc.c", "build/talloc.o"),
    ]
    if not is_windows():
        compiler_sources.append(("src/external/talloc/lib/talloc/talloc.c", "build/talloc_lib.o"))

    for source, output in compiler_sources:
        if not compile_object(root, gcc, flags, source, output):
            return False

    compiler_objs = [output for _, output in compiler_sources]
    link_cmd = [gcc, *flags, "-o", compiler_path(root), *[root / obj for obj in compiler_objs]]
    if not is_windows():
        link_cmd.append("-ldl")
    if run(link_cmd, root, "Compiler link failed") != 0:
        return False

    compiler = compiler_path(root)
    runtime_modules = [
        ("std", "src/std/std.c", "build/std.co.c", "build/std_manual.o", "build/std_gen.o", "build/std.o"),
        ("string", "src/string/string.c", "build/string.co.c", "build/string_manual.o", "build/string_gen.o", "build/string.o"),
    ]
    for module, manual_c, generated_c, manual_o, generated_o, combined_o in runtime_modules:
        if run([compiler, "genc", f"src/{module}/{module}.co", "-o", generated_c], root, f"Generate failed: {module}") != 0:
            return False
        if not compile_object(root, gcc, flags, manual_c, manual_o):
            return False
        if not compile_object(root, gcc, flags, generated_c, generated_o):
            return False
        if not combine_objects(root, combined_o, [manual_o, generated_o]):
            return False

    print(f"    [{GREEN}PASS{NC}] Rebuild complete")
    return True


def run_e2e_tests(root):
    test_files = sorted((root / "tests" / "test_files").glob("*.co"))
    if not test_files:
        print("No test files found in tests/test_files/")
        return True

    compiler = compiler_path(root)
    if not compiler.exists():
        print(f"    [{RED}FAIL{NC}] Compiler not found at {compiler}")
        return False

    passed = 0
    failed = 0
    with tempfile.TemporaryDirectory(prefix="come-e2e-") as td:
        tmp = Path(td)
        for source_file in test_files:
            print(source_file.name)
            expected = []
            for line in source_file.read_text(encoding="utf-8").splitlines():
                if "// EXPECT:" in line:
                    expected.append(line.split("// EXPECT:", 1)[1].strip())

            if not expected:
                print(f"    [SKIP] No expected output defined in {rel(root, source_file)}")
                passed += 1
                continue

            bin_name = tmp / f"{source_file.stem}{exe_suffix()}"
            if run([compiler, "build", source_file, "-o", bin_name], root, "Compilation failed") != 0:
                failed += 1
                continue

            proc = subprocess.run(
                [str(bin_name)],
                cwd=str(root),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
            )
            output = proc.stdout.strip()
            expected_str = "\n".join(expected)
            if proc.returncode == 0 and output == expected_str:
                print(f"    [{GREEN}PASS{NC}]")
                passed += 1
            else:
                print(f"    [{RED}FAIL{NC}] Output mismatch")
                print(f"      Expected:\n{expected_str}")
                print(f"      Got:\n{output}")
                failed += 1

    print(f"\nE2E Summary: {passed} passed, {failed} failed")
    return failed == 0


def expected_output_for(source):
    expected = []
    for line in Path(source).read_text(encoding="utf-8").splitlines():
        if "// EXPECT:" in line:
            expected.append(line.split("// EXPECT:", 1)[1].strip())
    return expected


def unit_specs(root):
    build_tests = root / "build" / "tests"
    return [
        (
            "test_lexer",
            [root / "tests" / "test_lexer.c", root / "src" / "core" / "lexer.c"],
            build_tests / f"test_lexer{exe_suffix()}",
        ),
        (
            "test_parser",
            [
                root / "tests" / "test_parser.c",
                root / "src" / "core" / "parser.c",
                root / "src" / "core" / "lexer.c",
            ],
            build_tests / f"test_parser{exe_suffix()}",
        ),
        (
            "test_codegen",
            [
                root / "tests" / "test_codegen.c",
                root / "src" / "core" / "parser.c",
                root / "src" / "core" / "lexer.c",
                root / "src" / "core" / "codegen.c",
                root / "src" / "core" / "utils.c",
            ],
            build_tests / f"test_codegen{exe_suffix()}",
        ),
        (
            "test_string",
            [
                root / "tests" / "test_string.c",
                root / "src" / "string" / "string.c",
                root / "src" / "mem" / "talloc.c",
                root / "src" / "core" / "utils.c",
            ],
            build_tests / f"test_string{exe_suffix()}",
        ),
    ]


def run_unit_tests(root):
    gcc = shutil.which("gcc")
    if not gcc:
        print(f"    [{RED}FAIL{NC}] gcc not found")
        return False

    build_tests = root / "build" / "tests"
    build_tests.mkdir(parents=True, exist_ok=True)

    include_flags = build_include_flags()

    passed = 0
    failed = 0
    for name, sources, output in unit_specs(root):
        print(name)
        cmd = [gcc, *include_flags, *sources]
        if name == "test_string" and not is_windows():
            cmd.append(root / "src" / "external" / "talloc" / "lib" / "talloc" / "talloc.c")
        cmd.extend(["-o", output])
        if not is_windows():
            cmd.append("-ldl")

        if run(cmd, root, "Compilation failed") != 0:
            failed += 1
            continue
        if run([output], root, "Execution failed") != 0:
            failed += 1
            continue

        print(f"    [{GREEN}PASS{NC}]")
        passed += 1

    print(f"\nUnit Summary: {passed} passed, {failed} failed")
    return failed == 0


def run_come_tests(root):
    compiler = compiler_path(root)
    if not compiler.exists():
        print(f"    [{RED}FAIL{NC}] Compiler not found at {compiler}")
        return False

    tests = sorted(list((root / "t").glob("*.co")) + list((root / "src").glob("**/t/*.co")))
    if not tests:
        print("No COME language tests found under t/ or src/**/t/")
        return True

    passed = 0
    failed = 0
    with tempfile.TemporaryDirectory(prefix="come-lang-") as td:
        tmp = Path(td)
        for source in tests:
            print(rel(root, source))
            group = "root" if source.parent == root / "t" else source.parent.parent.name
            bin_path = tmp / f"{group}_{source.stem}{exe_suffix()}"
            if run([compiler, "build", source, "-o", bin_path], root, "Compilation failed") != 0:
                failed += 1
                continue

            expected = expected_output_for(source)
            proc = subprocess.run(
                [str(bin_path)],
                cwd=str(root),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
            )

            if expected:
                output = proc.stdout.strip()
                expected_str = "\n".join(expected)
                if proc.returncode == 0 and output == expected_str:
                    print(f"    {GREEN}PASS{NC}")
                    passed += 1
                else:
                    print(f"    [{RED}FAIL{NC}] Output mismatch")
                    print(f"      Expected:\n{expected_str}")
                    print(f"      Got:\n{output}")
                    failed += 1
                continue

            if proc.stdout:
                for line in proc.stdout.splitlines():
                    if line.strip():
                        print(f"    {line}")
            if proc.returncode != 0:
                print(f"    [{RED}FAIL{NC}] Runtime failed")
                failed += 1
                continue
            print(f"    {GREEN}PASS{NC}")
            passed += 1

    print(f"\nCOME Summary: {passed} passed, {failed} failed")
    return failed == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--unit", action="store_true")
    parser.add_argument("--e2e", action="store_true")
    parser.add_argument("--come", action="store_true")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--rebuild", action="store_true", help="rebuild compiler/runtime before running tests")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    selected = args.all or not (args.unit or args.e2e or args.come)

    ok = True
    print(f"Platform: {platform.system()}")
    needs_compiler = selected or args.e2e or args.come
    if args.rebuild or (needs_compiler and not build_artifacts_ready(root)):
        ok = rebuild_project(root) and ok
    if selected or args.e2e:
        ok = run_e2e_tests(root) and ok
    if selected or args.come:
        ok = run_come_tests(root) and ok
    if selected or args.unit:
        ok = run_unit_tests(root) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
