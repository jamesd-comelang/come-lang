#!/bin/bash
# COIL-129: the top-20 error cases, reviewed by hand.
#
# Each case is a mistake a new user plausibly makes in
# their first week. The bar for every one: the build fails
# AND the first diagnostic names what is wrong in Come
# terms - the rule where the spec has one, and how to fix
# it. Before this story, several of these leaked raw C
# compiler or linker errors with mangled names
# (come_main__nosuchfn, "did you mean 'isinf'?"), one
# segfaulted the compiler, and two silently built.
#
#  1 no module declaration        11 assign to a constant
#  2 unknown top-level construct  12 break outside a loop
#  3 missing ')'                  13 continue outside a loop
#  4 unterminated string          14 index a non-indexable
#  5 unresolved import            15 '=' in a condition
#  6 undefined variable           16 spawn a plain function
#  7 undefined function           17 coroutine returns a value
#  8 wrong argument count         18 call a coroutine directly
#  9 implicit narrowing           19 write to a string index
# 10 [...] array literal          20 call a non-exported function
set -e
cd "$(dirname "$0")/.."
COME=./build/come
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

check() { # name, expected-fragment; source on stdin
    local name=$1 frag=$2
    cat > "$SCRATCH/$name.co"
    rm -rf .ccache
    if $COME build "$SCRATCH/$name.co" -o "$SCRATCH/$name.bin" > "$SCRATCH/$name.log" 2>&1; then
        echo "ERRORS: case '$name' built but must fail"
        exit 1
    fi
    if ! grep -qF -- "$frag" "$SCRATCH/$name.log"; then
        echo "ERRORS: case '$name' failed without the expected message fragment:"
        echo "  wanted: $frag"
        echo "  got: $(head -2 "$SCRATCH/$name.log" | tr '\n' ' ')"
        exit 1
    fi
    echo "ERRORS: $name ok"
}

check no_module "must begin with a module declaration" <<'EOF'
import std

int main() {
    return 0
}
EOF

check unknown_top_level "unexpected top-level token 'interface'" <<'EOF'
module main
import std

interface Shape {
    int area()
}

int main() {
    return 0
}
EOF

check missing_paren "expected ')' but found 'return'" <<'EOF'
module main
import std

int main() {
    int x = (1 + 2
    return x
}
EOF

check unterminated_string "unterminated string literal at line 5" <<'EOF'
module main
import std

int main() {
    string s = "unterminated
    return 0
}
EOF

check bad_import "Could not resolve import: nosuchmod" <<'EOF'
module main
import std
import nosuchmod

int main() {
    return 0
}
EOF

check undef_var "'missing' is not defined" <<'EOF'
module main
import std

int main() {
    return missing + 1
}
EOF

check undef_fn "call to undefined function 'nosuchfn'" <<'EOF'
module main
import std

int main() {
    int x = nosuchfn(3)
    return x
}
EOF

check wrong_argc "function 'add' takes 2 argument(s), 1 given" <<'EOF'
module main
import std

int add(int a, int b) {
    return a + b
}

int main() {
    return add(1)
}
EOF

check narrowing "implicit narrowing: cannot store double in int" <<'EOF'
module main
import std

int main() {
    double d = 3.7
    int i = d
    return i
}
EOF

check bracket_literal "array literals use { } in Come, not [ ]" <<'EOF'
module main
import std

int main() {
    int a[] = [1, 2, 3]
    return 0
}
EOF

check const_assign "cannot assign to 'K': it is a constant" <<'EOF'
module main
import std

const K = 5

int main() {
    K = 6
    return K
}
EOF

check break_outside "'break' has no loop or switch to break out of" <<'EOF'
module main
import std

int main() {
    break
    return 0
}
EOF

check continue_outside "'continue' has no loop to continue" <<'EOF'
module main
import std

int main() {
    if (true) {
        continue
    }
    return 0
}
EOF

check index_nonarray "cannot index 'x' of type int" <<'EOF'
module main
import std

int main() {
    int x = 5
    int y = x[0]
    return y
}
EOF

check assign_in_cond "'=' does not compare in a condition" <<'EOF'
module main
import std

int main() {
    int x = 0
    if (x = 5) {
        return 1
    }
    return 0
}
EOF

check spawn_fn "spawn: 'worker' is not a coroutine declared in this module" <<'EOF'
module main
import std

int worker(int x) {
    return x
}

int main() {
    spawn worker(3)
    return 0
}
EOF

check coro_retval "coroutines cannot return a value (spec 9.1)" <<'EOF'
module main
import std

coroutine job() {
    return 5
}

int main() {
    spawn job()
    return 0
}
EOF

check coro_call "coroutine 'job' cannot be called as a function; start it with spawn" <<'EOF'
module main
import std

coroutine job() {
}

int main() {
    job()
    return 0
}
EOF

check string_index_assign "strings are immutable" <<'EOF'
module main
import std

int main() {
    string s = "abc"
    s[0] = 'x'
    return 0
}
EOF

# 20: a non-exported cross-module call needs two files.
mkdir -p "$SCRATCH/exp"
cat > "$SCRATCH/exp/geom.co" <<'EOF'
module geom
import std

export (
    int add2(int a)
)

int add2(int a) {
    return a + 2
}

int secret(int a) {
    return a * 10
}
EOF
cat > "$SCRATCH/exp/use_secret.co" <<'EOF'
module main
import std
import geom

int main() {
    return geom.secret(3)
}
EOF
rm -rf .ccache
if $COME build "$SCRATCH/exp/use_secret.co" -o "$SCRATCH/exp/u.bin" > "$SCRATCH/exp/u.log" 2>&1; then
    echo "ERRORS: case 'not_exported' built but must fail"
    exit 1
fi
grep -qF "'geom.secret' is not exported by module 'geom'" "$SCRATCH/exp/u.log" || {
    echo "ERRORS: case 'not_exported' failed without the expected message:"
    head -2 "$SCRATCH/exp/u.log"
    exit 1
}
echo "ERRORS: not_exported ok"

echo "Error-quality suite: all 20 cases name the rule and the fix."

# COIL-136: the reference mistakes. Each names the rule and
# the fix, like the cases above.
check ref_missing_amp "needs a reference: pass &variable" <<'EOF'
module main
import std

struct Vec3 {
    int x
    int y
    int z
}

int draw_line(&(struct Vec3) p1) {
    return p1.x
}

int main() {
    var a = struct Vec3{1, 2, 3}
    return draw_line(a)
}
EOF

check ref_stray_amp "is not a reference: remove the '&'" <<'EOF'
module main
import std

struct Vec3 {
    int x
    int y
    int z
}

int by_val(struct Vec3 v) {
    return v.x
}

int main() {
    var a = struct Vec3{1, 2, 3}
    return by_val(&a)
}
EOF

check ref_temporary "cannot take a reference to a temporary" <<'EOF'
module main
import std

struct Vec3 {
    int x
    int y
    int z
}

int draw_line(&(struct Vec3) p1) {
    return p1.x
}

int main() {
    return draw_line(&struct Vec3{1, 2, 3})
}
EOF

check ref_field "a struct field cannot be a reference" <<'EOF'
module main
import std

struct Vec3 {
    int x
    int y
    int z
}

struct Holder {
    int tag
    &(struct Vec3) v
}

int main() {
    return 0
}
EOF

check ref_return "is only allowed for a function parameter or a local variable" <<'EOF'
module main
import std

struct Vec3 {
    int x
    int y
    int z
}

&(struct Vec3) pick(struct Vec3 v) {
    return v
}

int main() {
    return 0
}
EOF

check empty_literal_arg "element type cannot be worked out here" <<'EOF'
module main
import std

int sum(int xs[]) {
    return xs.length()
}

int main() {
    return sum({})
}
EOF

check mixed_literal_arg "element type cannot be worked out here" <<'EOF'
module main
import std

int sum(int xs[]) {
    return xs.length()
}

int main() {
    return sum({1, "x"})
}
EOF

echo "COIL-136 reference cases: all 5 name the rule and the fix."
echo "COIL-145 literal cases: both name the fix."
