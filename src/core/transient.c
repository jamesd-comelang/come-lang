// Design 1+2 (redesign), straight-line slice: transient string
// locals whose concat initializer can stay in the bump arena.
//
// A string local is transient (this slice) when ALL of:
//   - declared with explicit type `string`, or `var` with a
//     string-concat initializer (so the type is provably string);
//   - the initializer is a string concat (BINARY_OP "+");
//   - the name is not a parameter name and is not
//     re-declared (shadowed) anywhere in the function;
//   - the initializer does not reference the name itself;
//   - the name is never assigned again anywhere in the body;
//   - every other identifier use of the name is a whitelisted
//     borrow: a concat operand, an operand of == / !=, the
//     receiver of a pure string method (length,
//     bytesize, owner, substr, view, byte_view), or an
//     argument of a std printf (out/err/std.printf --
//     printf only reads its arguments).
// Loop-body locals (step 4a): a local DECLARED inside a
// loop body is a candidate -- its frame pops per
// iteration, freeing its arena string, and each
// iteration re-declares it fresh, so no value can
// survive into a later iteration. An OUTER local
// assigned inside a loop stays talloc, but that falls
// out of the never-reassigned rule (the assignment is
// a disqualifying use), not out of a loop exclusion.
// Anything else -- return, call argument (other than
// std printf), store, map/array/field value,
// dup/chown receiver, spawn/yield capture --
// disqualifies the local, and the local keeps the
// ordinary talloc path. The default is
// "not transient": an unrecognized construct can only
// miss an optimization, never dangle a value.
//
// Why this is sound: a transient local's arena concat is
// created in the same block as the declaration (the init
// is evaluated there), and the local is never stored,
// returned, or captured, so no reference to the arena
// block can outlive the block's frame pop, which frees
// it. Borrows (concat operands copy bytes, pure methods
// read) are all evaluated while the block is live.
//
// Note on the stringish check: it is only a filter to
// keep the registry clean, not a soundness gate. Codegen
// selects the string-concat branch by its own type logic;
// the flag this analysis controls is irrelevant to a
// numeric "+" (it never reaches the concat emission).
// So even an imprecise stringish answer cannot
// miscompile -- it can only register a local whose init
// codegen then treats as non-string (flag ignored).
//
// The registry is (module, function, local name), filled
// by codegen_analyze_transient during the driver's
// pre-scan and consulted by codegen at VAR_DECL time
// (codegen_transient_local). A lookup miss means the
// ordinary path, so modules the driver did not analyze
// are unaffected.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

#define TRANS_MAX_NAMES 512
#define TRANS_MAX_CANDS 256
#define TRANS_REG_MAX 4096

typedef struct { char module[128]; char fn[128]; char name[128]; } TransReg;
static TransReg g_trans_reg[TRANS_REG_MAX];
static int g_trans_reg_count = 0;

static void trans_reg_add(const char* module, const char* fn, const char* name) {
    for (int i = 0; i < g_trans_reg_count; i++)
        if (strcmp(g_trans_reg[i].module, module) == 0 &&
            strcmp(g_trans_reg[i].fn, fn) == 0 &&
            strcmp(g_trans_reg[i].name, name) == 0) return;
    if (g_trans_reg_count >= TRANS_REG_MAX) return;
    TransReg* r = &g_trans_reg[g_trans_reg_count++];
    snprintf(r->module, sizeof(r->module), "%s", module);
    snprintf(r->fn, sizeof(r->fn), "%s", fn);
    snprintf(r->name, sizeof(r->name), "%s", name);
}

int codegen_transient_local(const char* module, const char* fn, const char* name) {
    if (!module || !fn || !name) return 0;
    for (int i = 0; i < g_trans_reg_count; i++)
        if (strcmp(g_trans_reg[i].module, module) == 0 &&
            strcmp(g_trans_reg[i].fn, fn) == 0 &&
            strcmp(g_trans_reg[i].name, name) == 0) return 1;
    return 0;
}

// Per-function collection state: the names declared so
// far (params first, then body VAR_DECLs in order), each
// with whether it is provably a string, plus the names
// that turned out to be shadowed.
typedef struct {
    char names[TRANS_MAX_NAMES][128];
    int is_string[TRANS_MAX_NAMES];
    int count;
    char shadowed[TRANS_MAX_NAMES][128];
    int shadowed_count;
    char params[TRANS_MAX_NAMES][128];
    int param_count;
    char cands[TRANS_MAX_CANDS][128];
    int cand_count;
} TransCtx;

static int trans_map_find(TransCtx* ctx, const char* name) {
    for (int i = 0; i < ctx->count; i++)
        if (strcmp(ctx->names[i], name) == 0) return i;
    return -1;
}

static int trans_is_param(TransCtx* ctx, const char* name) {
    for (int i = 0; i < ctx->param_count; i++)
        if (strcmp(ctx->params[i], name) == 0) return 1;
    return 0;
}

static int trans_is_shadowed(TransCtx* ctx, const char* name) {
    for (int i = 0; i < ctx->shadowed_count; i++)
        if (strcmp(ctx->shadowed[i], name) == 0) return 1;
    return 0;
}

static void trans_map_add(TransCtx* ctx, const char* name, int is_string) {
    if (trans_map_find(ctx, name) >= 0) {
        // Re-declaration: the name is shadowed from here
        // on, so no candidate may use it.
        if (ctx->shadowed_count < TRANS_MAX_NAMES)
            snprintf(ctx->shadowed[ctx->shadowed_count++], 128, "%s", name);
        return;
    }
    if (ctx->count >= TRANS_MAX_NAMES) return;
    snprintf(ctx->names[ctx->count], 128, "%s", name);
    ctx->is_string[ctx->count] = is_string;
    ctx->count++;
}

// Does the subtree contain an identifier with this name?
static int trans_name_in_subtree(ASTNode* node, const char* name) {
    if (!node) return 0;
    if (node->type == AST_IDENTIFIER && strcmp(node->text, name) == 0) return 1;
    for (int i = 0; i < node->child_count; i++)
        if (trans_name_in_subtree(node->children[i], name)) return 1;
    return 0;
}

// Is this expression provably a string? Literals, string
// identifiers (from the map), string-producing method
// calls, and concats of stringish operands. Anything
// else is unknown (0) -- conservative, and harmless (see
// the header note: the flag is ignored for non-string
// concats anyway).
static int trans_expr_is_string(ASTNode* node, TransCtx* ctx) {
    if (!node) return 0;
    if (node->type == AST_STRING_LITERAL) return 1;
    if (node->type == AST_IDENTIFIER) {
        int idx = trans_map_find(ctx, node->text);
        return idx >= 0 && ctx->is_string[idx];
    }
    if (node->type == AST_BINARY_OP && strcmp(node->text, "+") == 0 &&
        node->child_count >= 2)
        return trans_expr_is_string(node->children[0], ctx) &&
               trans_expr_is_string(node->children[1], ctx);
    if (node->type == AST_METHOD_CALL) {
        const char* m = node->text;
        if (strcmp(m, "substr") == 0 || strcmp(m, "view") == 0 ||
            strcmp(m, "byte_view") == 0 || strcmp(m, "dup") == 0 ||
            strcmp(m, "upper") == 0 || strcmp(m, "lower") == 0 ||
            strcmp(m, "trim") == 0 || strcmp(m, "ltrim") == 0 ||
            strcmp(m, "rtrim") == 0 || strcmp(m, "repeat") == 0 ||
            strcmp(m, "join") == 0 || strcmp(m, "replace") == 0) return 1;
        return 0;
    }
    return 0;
}

static int trans_is_pure_receiver_method(const char* m) {
    return strcmp(m, "length") == 0 || strcmp(m, "bytesize") == 0 ||
           strcmp(m, "owner") == 0 || strcmp(m, "substr") == 0 ||
           strcmp(m, "view") == 0 || strcmp(m, "byte_view") == 0;
}

// Collect candidates: walk the body in order, tracking
// the declaration map. A VAR_DECL is a candidate with
// a concat init, a provably string type, no
// self-reference, no param collision, and no shadowing
// (checked at the end, since a later re-declaration
// shadows an earlier candidate). Loop-body locals are
// candidates (step 4a, see the header note); the loop
// rule is enforced by the never-reassigned check, not
// by where the declaration sits.
static void trans_collect(ASTNode* node, TransCtx* ctx) {
    if (!node) return;
    if (node->type == AST_VAR_DECL) {
        const char* type = (node->child_count >= 2 && node->children[1])
            ? node->children[1]->text : "var";
        ASTNode* init = (node->child_count >= 1) ? node->children[0] : NULL;
        int is_string = strcmp(type, "string") == 0;
        if (init && init->type == AST_BINARY_OP &&
            strcmp(init->text, "+") == 0 &&
            (is_string || strcmp(type, "var") == 0) &&
            trans_expr_is_string(init, ctx) &&
            !trans_name_in_subtree(init, node->text) &&
            !trans_is_param(ctx, node->text) &&
            ctx->cand_count < TRANS_MAX_CANDS)
            snprintf(ctx->cands[ctx->cand_count++], 128, "%.127s", node->text);
        // Record the declaration for later operand checks.
        // A `var` with a string-concat init locks to string
        // (spec 10.1, COIL-127), so it counts as string.
        int as_string = is_string;
        if (!as_string && strcmp(type, "var") == 0 && init &&
            init->type == AST_BINARY_OP && strcmp(init->text, "+") == 0 &&
            trans_expr_is_string(init, ctx)) as_string = 1;
        trans_map_add(ctx, node->text, as_string);
    }
    for (int i = 0; i < node->child_count; i++)
        trans_collect(node->children[i], ctx);
}

// Disqualifier scan: 1 if the body uses `name` anywhere
// except as a whitelisted borrow. Generic recursion over
// all children, so no construct is missed; the parent
// node and child index classify each identifier use.
static int trans_use_bad(ASTNode* node, const char* name) {
    if (!node) return 0;
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* ch = node->children[i];
        if (!ch) continue;
        if (ch->type == AST_IDENTIFIER && strcmp(ch->text, name) == 0) {
            int ok = 0;
            if (node->type == AST_BINARY_OP &&
                (strcmp(node->text, "+") == 0 ||
                 strcmp(node->text, "==") == 0 ||
                 strcmp(node->text, "!=") == 0)) ok = 1;
            else if (node->type == AST_METHOD_CALL && i == 0 &&
                     trans_is_pure_receiver_method(node->text)) ok = 1;
            else if (node->type == AST_METHOD_CALL && i >= 1 &&
                     strcmp(node->text, "printf") == 0 &&
                     node->child_count >= 1 &&
                     // Receiver text only, like codegen's own
                     // printf branch: std.out parses as a
                     // field node, not an identifier, so a
                     // type check here would miss it.
                     (strcmp(node->children[0]->text, "out") == 0 ||
                      strcmp(node->children[0]->text, "err") == 0 ||
                      strcmp(node->children[0]->text, "std") == 0)) ok = 1;
            if (!ok) return 1;
        }
        if (trans_use_bad(ch, name)) return 1;
    }
    return 0;
}

void codegen_analyze_transient(const char* module, ASTNode** asts, int nast) {
    if (!module) return;
    for (int a = 0; a < nast; a++) {
        ASTNode* ast = asts[a];
        if (!ast || ast->type != AST_PROGRAM) continue;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* ch = ast->children[i];
            if (!ch || ch->type != AST_FUNCTION || !ch->text[0]) continue;
            ASTNode* body = NULL;
            TransCtx ctx;
            memset(&ctx, 0, sizeof(ctx));
            for (int j = 0; j < ch->child_count; j++) {
                ASTNode* fch = ch->children[j];
                if (!fch) continue;
                if (fch->type == AST_VAR_DECL) {
                    const char* t = (fch->child_count >= 2 && fch->children[1])
                        ? fch->children[1]->text : "var";
                    if (ctx.param_count < TRANS_MAX_NAMES)
                        snprintf(ctx.params[ctx.param_count++], 128, "%.127s", fch->text);
                    trans_map_add(&ctx, fch->text, strcmp(t, "string") == 0);
                } else if (fch->type == AST_BLOCK) {
                    body = fch;
                }
            }
            if (!body) continue;
            trans_collect(body, &ctx);
            for (int c = 0; c < ctx.cand_count; c++) {
                const char* name = ctx.cands[c];
                if (trans_is_shadowed(&ctx, name)) continue;
                if (trans_is_param(&ctx, name)) continue;
                if (trans_use_bad(body, name)) continue;
                trans_reg_add(module, ch->text, name);
            }
        }
    }
}
