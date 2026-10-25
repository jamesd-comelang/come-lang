// come_err.h - the ERR record (spec 12, COIL-139).
//
// Every error the runtime sets is recorded as an entry in a
// fixed-size chain: code, message, and the source location
// (module, function, file, line) where it was set. Index 0
// is the root cause - the first error since the last clear.
// The store is a fixed static table: no heap, no allocation.
//
// The record is per execution context: main has one, and
// each coroutine has its own, swapped by the scheduler, so
// errors in one coroutine are never visible in another.
//
// Set sites call come_ERR_set (mem/talloc.h) as before; the
// recording rides along. Sites in generated code first arm
// a pending location with come_err_arm(); the next recorded
// error consumes it. Runtime slots that know their caller's
// site directly (the out-of-bounds slots) call
// come_err_record_at instead.
#ifndef COME_ERR_H
#define COME_ERR_H

#define COME_ERR_CHAIN_MAX 16

typedef struct {
    int code;
    int line;
    char msg[160];
    char module[48];
    char func[64];
    char file[224];
} ComeErrEntry;

typedef struct {
    ComeErrEntry entries[COME_ERR_CHAIN_MAX];
    int depth;
    // The legacy specific-message slot (COIL-118), kept per
    // record so ERR.str()'s fallback agrees with the chain.
    int custom_code;
    char custom[256];
    // errno belonging to this record, swapped with it.
    int saved_errno;
    // Pending source location, armed by generated code
    // before a call/index that may set an error. The
    // strings are pointers, not copies: every arming
    // site passes string literals (codegen emits them),
    // so storing the pointer records the same location
    // without three snprintfs per executed statement.
    // (COIL-158: the copies were the tokenizer's biggest
    // single cost - an arm fires before every statement.)
    int armed;
    int arm_line;
    const char *arm_module;
    const char *arm_func;
    const char *arm_file;
} ComeErrRecord;

// The record for the current execution context.
ComeErrRecord *come_err_active(void);

// Make rec the active record, saving errno into the old
// record and restoring rec's. Used by the coroutine
// scheduler; main never calls it.
void come_err_activate(ComeErrRecord *rec);

// Arm the pending location for the next recorded error.
#ifndef COME_ERR_NO_ARM_PROTO
void come_err_arm(const char *module, const char *func,
                  const char *file, int line);
#endif

// Record an error with an explicit C-side location
// (file/line/func of the setting site, e.g. from a macro's
// __FILE__/__LINE__/__func__ in generated code). cfunc is
// demangled when it has the come_<module>__<func> shape.
void come_err_record_at(int code, const char *msg,
                        const char *file, int line,
                        const char *cfunc);

// Reset the active record (ERR.clear()).
void come_err_reset(void);

// Accessors. An out-of-range index appends a new error
// entry (ERANGE, located at the access site) and the read
// returns the zero value: 0, "", or "" respectively.
int come_err_depth(void);
int come_err_code_at(int i, const char *module, const char *func,
                     const char *file, int line);
const char *come_err_msg_at(int i, const char *module, const char *func,
                            const char *file, int line);
// Formats "<file>:<line>" of entry i into buf.
const char *come_err_where_at(int i, char *buf, unsigned long bufsz,
                              const char *module, const char *func,
                              const char *file, int line);
// "<module>.<func> at <file>:<line>" of entry i, for
// ERR.fatal's report. Returns "" when i is out of range.
const char *come_err_describe_at(int i, char *buf, unsigned long bufsz);

#endif
