#!/usr/bin/env python3
import os
import subprocess
import sys
import glob

# Colors
NC = "\x1b[0m"
RED = "\x1b[1;38;2;255;255;255;48;2;200;0;0m"
GREEN = "\x1b[1;38;2;255;255;255;48;2;0;150;0m"

def run_test(source_file):
    test_name = os.path.basename(source_file)
    print(f"{test_name}")
    
    # Read expected output
    expected_output = []
    with open(source_file, 'r') as f:
        for line in f:
            if "// EXPECT:" in line:
                expected_output.append(line.split("// EXPECT:")[1].strip())
    
    if not expected_output:
        print(f"    [SKIP] No expected output defined in {source_file}")
        return True

    # Compile
    compiler = "./build/come"
    if not os.path.exists(compiler):
        print(f"    [{RED}FAIL{NC}] Compiler not found at {compiler}")
        return False

    bin_name = source_file.replace(".co", "")
    compile_cmd = [compiler, "build", source_file, "-o", bin_name]
    
    try:
        # Capture output and indent it
        proc_output = subprocess.check_output(compile_cmd, stderr=subprocess.STDOUT).decode()
        for line in proc_output.splitlines():
            if line.strip():
                print(f"    {line}")
    except subprocess.CalledProcessError as e:
        for line in e.output.decode().splitlines():
            if line.strip():
                print(f"    {line}")
        print(f"    [{RED}FAIL{NC}] Compilation failed")
        return False

    # Run
    try:
        output = subprocess.check_output([bin_name], stderr=subprocess.STDOUT).decode().strip()
    except subprocess.CalledProcessError as e:
        for line in e.output.decode().splitlines():
            if line.strip():
                print(f"    {line}")
        print(f"    [{RED}FAIL{NC}] Execution failed")
        return False
    finally:
        if os.path.exists(bin_name):
            os.remove(bin_name)

    # Verify
    expected_str = "\n".join(expected_output)
    if output == expected_str:
        print(f"    [{GREEN}PASS{NC}]")
        return True
    else:
        print(f"    [{RED}FAIL{NC}] Output mismatch")
        print(f"      Expected:\n{expected_str}")
        print(f"      Got:\n{output}")
        return False

def main():
    test_files = glob.glob("tests/test_files/*.co")
    if not test_files:
        print("No test files found in tests/test_files/")
        return 0

    passed = 0
    failed = 0
    
    for f in test_files:
        if run_test(f):
            passed += 1
        else:
            failed += 1
            
    print(f"\nSummary: {passed} passed, {failed} failed")
    if failed > 0:
        sys.exit(1)
    sys.exit(0)

if __name__ == "__main__":
    main()
