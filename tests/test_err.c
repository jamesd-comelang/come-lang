// COIL-139: the ERR record - a fixed-size chain of errors
// with source locations, per execution context.
//
// Pins: recording via come_ERR_set (armed site consumed),
// direct recording with demangling, the root-cause index,
// out-of-range accessor behavior (zero value + a new
// entry), the chain cap (root and newest survive), reset,
// and per-context isolation via come_err_activate.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "come_err.h"
#include "mem/talloc.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void) {
    char buf[512];

    // Empty record: depth 0, legacy reads, no recording.
    come_err_reset();
    CHECK(come_err_depth() == 0, "depth 0 after reset");
    errno = 0;
    CHECK(come_err_code_at(0, "m", "f", "x.co", 1) == 0, "code_at(0) empty is errno");
    CHECK(come_err_depth() == 0, "legacy read does not record");

    // Armed site is consumed by the next come_ERR_set.
    come_err_arm("shop", "checkout", "shop.co", 42);
    come_ERR_set(ENOENT, "map key not found");
    CHECK(come_err_depth() == 1, "depth 1 after one error");
    CHECK(come_err_code_at(0, "", "", "", 0) == ENOENT, "root code");
    CHECK(strcmp(come_err_msg_at(0, "", "", "", 0), "map key not found") == 0, "root msg");
    come_err_where_at(0, buf, sizeof(buf), "", "", "", 0);
    CHECK(strcmp(buf, "shop.co:42") == 0, "where names the armed file:line");
    come_err_describe_at(0, buf, sizeof(buf));
    CHECK(strcmp(buf, "shop.checkout at shop.co:42") == 0, "describe root");

    // A second error appends; the root does not move. Its
    // site is empty: nothing armed it.
    come_ERR_set(ERANGE, "array index out of bounds");
    CHECK(come_err_depth() == 2, "depth 2 after two errors");
    CHECK(come_err_code_at(0, "", "", "", 0) == ENOENT, "root preserved");
    CHECK(come_err_code_at(1, "", "", "", 0) == ERANGE, "second code");
    come_err_where_at(1, buf, sizeof(buf), "", "", "", 0);
    CHECK(buf[0] == 0, "unarmed set has no location");

    // Direct recording demangles the C function name.
    come_err_record_at(EPIPE, "link is closed", "net.co", 7, "come_net__pump");
    CHECK(come_err_depth() == 3, "depth 3");
    come_err_describe_at(2, buf, sizeof(buf));
    CHECK(strcmp(buf, "net.pump at net.co:7") == 0, "record_at demangles");

    // Out-of-range index: zero values, and a NEW error entry
    // located at the access site; the root is untouched.
    int before = come_err_depth();
    CHECK(come_err_code_at(9, "main", "main", "probe.co", 99) == 0, "OOR code returns 0");
    CHECK(come_err_depth() == before + 1, "OOR appends an entry");
    CHECK(come_err_code_at(0, "", "", "", 0) == ENOENT, "root survives OOR");
    CHECK(come_err_code_at(before, "", "", "", 0) == ERANGE, "OOR entry code");
    come_err_where_at(before, buf, sizeof(buf), "", "", "", 0);
    CHECK(strcmp(buf, "probe.co:99") == 0, "OOR entry located at the access");
    CHECK(strcmp(come_err_msg_at(9, "", "", "", 0), "") == 0, "OOR msg returns empty");

    // Negative index is out of range too.
    before = come_err_depth();
    come_err_where_at(-1, buf, sizeof(buf), "main", "main", "probe.co", 100);
    CHECK(buf[0] == 0, "negative where returns empty");
    CHECK(come_err_depth() == before + 1, "negative appends");

    // The cap: root and the newest survive, depth saturates.
    come_err_reset();
    for (int i = 0; i < COME_ERR_CHAIN_MAX + 4; i++) {
        come_ERR_set(1000 + i, "flood");
    }
    CHECK(come_err_depth() == COME_ERR_CHAIN_MAX, "depth saturates at the cap");
    CHECK(come_err_code_at(0, "", "", "", 0) == 1000, "root survives the flood");
    CHECK(come_err_code_at(COME_ERR_CHAIN_MAX - 1, "", "", "", 0) == 1000 + COME_ERR_CHAIN_MAX + 3,
          "newest replaces the last slot");

    // ERR.clear() also resets errno and the specific message.
    come_ERR_set(EINVAL, "custom text");
    CHECK(come_ERR_message_for(EINVAL) != NULL, "custom message rides along");
    come_err_reset();
    errno = 0;
    CHECK(come_err_depth() == 0, "reset clears depth");
    CHECK(come_ERR_message_for(EINVAL) == NULL, "reset clears custom");

    // Per-context isolation: two records, swapped.
    static ComeErrRecord rec_a, rec_b;
    memset(&rec_a, 0, sizeof(rec_a));
    memset(&rec_b, 0, sizeof(rec_b));
    ComeErrRecord *main_rec = come_err_active();
    come_err_activate(&rec_a);
    come_ERR_set(ENOENT, "in a");
    CHECK(come_err_depth() == 1, "a has one error");
    come_err_activate(&rec_b);
    CHECK(come_err_depth() == 0, "b does not see a's error");
    CHECK(errno == 0, "errno swapped with the record");
    come_ERR_set(ERANGE, "in b");
    come_err_activate(&rec_a);
    CHECK(come_err_depth() == 1 && come_err_code_at(0, "", "", "", 0) == ENOENT,
          "a's record intact");
    CHECK(errno == ENOENT, "a's errno restored");
    come_err_activate(main_rec);
    come_err_reset();
    errno = 0;

    if (failures) { printf("ERR: %d failures\n", failures); return 1; }
    printf("PASS: err record\n");
    return 0;
}
