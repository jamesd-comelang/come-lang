#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "parser.h"
#include "lexer.h"

// Forward declarations
static void parse_top_level_decl(ASTNode* program);
static int is_type_token(TokenType type);

static TokenList tokens;
static int pos;

// Fatal parse errors (parse_file returns nonzero when any occurred).
// Most parser diagnostics only print and parsing limps on; errors that
// must fail the build - like the removed [...] literal (COIL-100) -
// increment this counter.
static int g_parse_errors = 0;
static int g_in_struct_fields = 0; // COIL-136: reject reference fields

// Alias Storage
typedef struct {
    char name[256];
    ASTNode* replacement;
} AliasEntry;

#define MAX_ALIASES 1024
static AliasEntry alias_table[MAX_ALIASES];
static int alias_count = 0;

static void register_alias(const char* name, ASTNode* replacement) {
    if (alias_count < MAX_ALIASES) {
        strcpy(alias_table[alias_count].name, name);
        alias_table[alias_count].replacement = replacement;
        alias_count++;
    } else {
        printf("Error: Too many aliases defined\n");
    }
}

// Type-alias names (spec 5.1). Type aliases live in the
// AST as typedefs, not in the substitution table above, so
// their names are kept here for one reason: a call-shaped
// use of a type-alias name is a cast - tcpport_t(8080) -
// not a function call (COIL-129; it used to emit a call to
// a nonexistent come_<module>__tcpport_t and fail in the
// linker).
static char type_alias_names[MAX_ALIASES][256];
static int type_alias_name_count = 0;

static void register_type_alias(const char* name) {
    for (int i = 0; i < type_alias_name_count; i++)
        if (strcmp(type_alias_names[i], name) == 0) return;
    if (type_alias_name_count < MAX_ALIASES)
        strcpy(type_alias_names[type_alias_name_count++], name);
}

static int is_type_alias_name(const char* name) {
    for (int i = 0; i < type_alias_name_count; i++)
        if (strcmp(type_alias_names[i], name) == 0) return 1;
    return 0;
}

// Macro aliases (spec 5.3): alias SQUARE(x) = ((x) * (x)).
// The body is stored as a template AST; a call site expands
// to a clone with the arguments substituted for the
// parameters (COIL-127).
#define MAX_MACROS 64
typedef struct {
    char name[64];
    char params[8][64];
    int param_count;
    ASTNode* body;
} MacroEntry;
static MacroEntry macro_table[MAX_MACROS];
static int macro_count = 0;

static MacroEntry* find_macro(const char* name) {
    for (int i = 0; i < macro_count; i++) {
        if (strcmp(macro_table[i].name, name) == 0) return &macro_table[i];
    }
    return NULL;
}

static ASTNode* ast_clone(ASTNode* node);

static void macro_substitute(ASTNode* n, MacroEntry* m, ASTNode* call) {
    if (!n) return;
    if (n->type == AST_IDENTIFIER) {
        for (int i = 0; i < m->param_count; i++) {
            if (strcmp(n->text, m->params[i]) == 0) {
                ASTNode* argcopy = ast_clone(call->children[i]);
                *n = *argcopy; // take over the argument's shape
                return; // never re-scan substituted arguments
            }
        }
    }
    for (int i = 0; i < n->child_count; i++) {
        macro_substitute(n->children[i], m, call);
    }
}

static ASTNode* find_alias(const char* name) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(alias_table[i].name, name) == 0) {
            return alias_table[i].replacement;
        }
    }
    return NULL;
}

static ASTNode* ast_clone(ASTNode* node) {
    if (!node) return NULL;
    ASTNode* copy = ast_new(node->type);
    strcpy(copy->text, node->text);
    copy->source_line = node->source_line;
    copy->array_size = node->array_size;
    copy->layout_packed = node->layout_packed;
    copy->layout_align = node->layout_align;
    copy->is_extern = node->is_extern;
    copy->child_count = node->child_count;
    for (int i = 0; i < node->child_count; i++) {
        copy->children[i] = ast_clone(node->children[i]);
    }
    return copy;
}

static Token* current() {
    if (pos >= tokens.count) return &tokens.tokens[tokens.count-1];
    return &tokens.tokens[pos];
}

static void advance() {
    if (pos < tokens.count) pos++;
}

static int match(TokenType type) {
    if (current()->type == type) {
        advance();
        return 1;
    }
    return 0;
}

// Human-readable token names for error messages (COIL-129:
// "Expected token type 25, got 32" told the user nothing).
static const char* token_name(TokenType type) {
    switch (type) {
        case TOKEN_EOF: return "end of file";
        case TOKEN_IDENTIFIER: return "a name";
        case TOKEN_NUMBER: return "a number";
        case TOKEN_STRING: return "a string";
        case TOKEN_LPAREN: return "'('";
        case TOKEN_RPAREN: return "')'";
        case TOKEN_LBRACE: return "'{'";
        case TOKEN_RBRACE: return "'}'";
        case TOKEN_LBRACKET: return "'['";
        case TOKEN_RBRACKET: return "']'";
        case TOKEN_COMMA: return "','";
        case TOKEN_SEMICOLON: return "';'";
        case TOKEN_COLON: return "':'";
        case TOKEN_DOT: return "'.'";
        case TOKEN_ASSIGN: return "'='";
        default: return "a token";
    }
}

static int expect(TokenType type) {
    if (match(type)) return 1;
    // A failed expectation is a parse error (COIL-129): it
    // used to print and recover, so programs with missing
    // brackets built successfully with pieces missing.
    printf("Error: expected %s but found '%s' at line %d\n",
           token_name(type), current()->text, current()->line);
    g_parse_errors++;
    return 0;
}

// Peek at an array declarator starting at the current token: when it
// is '[' NUMBER ']', return the fixed size; otherwise 0 (dynamic).
// The size used to be discarded while skipping the brackets, which
// made fixed arrays indistinguishable from dynamic ones (COIL-101).
static int peek_array_size() {
    if (current()->type == TOKEN_LBRACKET && pos + 1 < tokens.count &&
        tokens.tokens[pos + 1].type == TOKEN_NUMBER) {
        return atoi(tokens.tokens[pos + 1].text);
    }
    // A constant alias as the size: alias MAX_ARRAY = 5 is
    // the spec 5.2 example, written for exactly this use.
    if (current()->type == TOKEN_LBRACKET && pos + 1 < tokens.count &&
        tokens.tokens[pos + 1].type == TOKEN_IDENTIFIER) {
        ASTNode* rep = find_alias(tokens.tokens[pos + 1].text);
        if (rep && rep->type == AST_NUMBER) return atoi(rep->text);
    }
    return 0;
}

ASTNode* ast_new(ASTNodeType type) {
    ASTNode* n = malloc(sizeof(ASTNode));
    n->type = type;
    n->child_count = 0;
    n->text[0] = '\0';
    n->source_line = (pos < tokens.count) ? tokens.tokens[pos].line : 0;
    n->array_size = 0;
    n->layout_packed = 0;
    n->layout_align = 0;
    n->is_extern = 0;
    n->is_coroutine = 0;
    return n;
}

void ast_free(ASTNode* node) {
    if (!node) return;
    for(int i=0;i<node->child_count;i++)
        ast_free(node->children[i]);
    free(node);
}

// Forward decls
static ASTNode* parse_block();
static ASTNode* parse_statement();
static ASTNode* parse_expression();
static ASTNode* parse_var_decl();

// Parse the body of a "{ ... }" literal after the opening brace
// has been consumed: positional values, .field = value pairs,
// or key: value pairs (COIL-102). The node's text is the
// caller's tag: "MAP" for a bare literal, "struct Name" for a
// typed struct literal (COIL-136).
static ASTNode* parse_brace_aggregate(const char* tag) {
    ASTNode* node = ast_new(AST_AGGREGATE_INIT);
    strcpy(node->text, tag);
    while (current()->type != TOKEN_RBRACE && current()->type != TOKEN_EOF) {
         if (match(TOKEN_DOT)) {
             if (current()->type == TOKEN_IDENTIFIER) {
                 ASTNode* desig = ast_new(AST_IDENTIFIER);
                 snprintf(desig->text, sizeof(desig->text), ".%.*s", 126, current()->text);
                 advance();
                 if (match(TOKEN_ASSIGN)) {
                     ASTNode* value = parse_expression();
                     ASTNode* pair = ast_new(AST_ASSIGN);
                     pair->children[pair->child_count++] = desig;
                     pair->children[pair->child_count++] = value;
                     node->children[node->child_count++] = pair;
                 }
             }
         } else {
             ASTNode* first = parse_expression();
             if (match(TOKEN_COLON)) {
                 // Map literal entry: key: value (COIL-102)
                 ASTNode* pair = ast_new(AST_MAP_PAIR);
                 pair->children[pair->child_count++] = first;
                 pair->children[pair->child_count++] = parse_expression();
                 node->children[node->child_count++] = pair;
             } else {
                 node->children[node->child_count++] = first;
             }
         }
         if (!match(TOKEN_COMMA)) break;
    }
    expect(TOKEN_RBRACE);
    return node;
}

static ASTNode* parse_primary() {
    ASTNode* node = NULL;
    Token* t = current();
    
    // Handle Unary Ops that are often parsed at primary level in simple parsers, 
    // though conceptually in expression_prec.
    // In this parser, `parse_expression_prec` calls `parse_primary`.
    // Unary ops like !, ~, * are handled here?
    // Snippet 159 showed NOT, TILDE, STAR handling at TOP of parse_primary!
    // I must preserve that.
    if (t->type == TOKEN_NOT || t->type == TOKEN_TILDE || t->type == TOKEN_STAR || t->type == TOKEN_MINUS || t->type == TOKEN_AND ||
        t->type == TOKEN_INC || t->type == TOKEN_DEC) {
        TokenType op_type = t->type;
        advance();
        ASTNode* operand = parse_primary(); // Recursive for **x or - -x
        ASTNode* unary = ast_new(AST_UNARY_OP);
        if (op_type == TOKEN_NOT) strcpy(unary->text, "!");
        else if (op_type == TOKEN_TILDE) strcpy(unary->text, "~");
        else if (op_type == TOKEN_STAR) strcpy(unary->text, "*");
        else if (op_type == TOKEN_AND) strcpy(unary->text, "&");
        else if (op_type == TOKEN_MINUS) strcpy(unary->text, "-");
        else if (op_type == TOKEN_INC) strcpy(unary->text, "++");
        else if (op_type == TOKEN_DEC) strcpy(unary->text, "--");
        unary->children[unary->child_count++] = operand;
        // Unary ops usually bind tight, but postfix binds tighter.
        // If I return here, I miss postfix on the result?
        // e.g. (*x).y
        // Standard C: *x.y means *(x.y) because . binds tighter than *.
        // So `parse_primary` should NOT loop for unary result?
        // Correct. *x.y: Postfix loop consumes x then .y. Then * is applied.
        // Wait, if I implement unary inside parse_primary, then I return `Unary(Recursive)`.
        // The recursive call consumes `x.y`.
        // So `*` applies to `x.y`. Correct.
        // So Unary case returns IMMEDIATELY.
        return unary;
    }

    // 1. Parse Atom
    // Typed struct literal (COIL-136): struct Name { ... } is a
    // value, e.g. var a = struct Vec3{1, 2, 3}.
    if (t->type == TOKEN_STRUCT && pos + 2 < tokens.count &&
        tokens.tokens[pos + 1].type == TOKEN_IDENTIFIER &&
        tokens.tokens[pos + 2].type == TOKEN_LBRACE) {
        char stag[300];
        snprintf(stag, sizeof(stag), "struct %.150s", tokens.tokens[pos + 1].text);
        advance(); // struct
        advance(); // Name
        advance(); // {
        node = parse_brace_aggregate(stag);
    } else if (t->type == TOKEN_IDENTIFIER) {
         // Check alias substitution
         ASTNode* alias_node = find_alias(t->text);
         if (alias_node) {
             node = ast_clone(alias_node);
             advance(); // Consume the alias identifier
         } else {
             node = ast_new(AST_IDENTIFIER);
             strcpy(node->text, t->text);
             advance();
         }
    } else if (t->type == TOKEN_STRING_LITERAL) {
        node = ast_new(AST_STRING_LITERAL);
        char combined[4096] = "";
        while (current()->type == TOKEN_STRING_LITERAL) {
             if (strlen(combined) + strlen(current()->text) >= sizeof(combined)) {
                 printf("Error: string literal too long (line %d, limit %d bytes)\n",
                        current()->line, (int)sizeof(combined) - 1);
                 g_parse_errors++;
                 advance();
                 break;
             }
             strcat(combined, current()->text);
             advance();
        }
        strcpy(node->text, combined);
    } else if (t->type == TOKEN_TRUE || t->type == TOKEN_FALSE) {
        node = ast_new(AST_BOOL_LITERAL);
        strcpy(node->text, t->text);
        advance();
    } else if (t->type == TOKEN_CHAR_LITERAL) {
        node = ast_new(AST_NUMBER);
        strcpy(node->text, t->text); 
        advance();
    } else if (t->type == TOKEN_NUMBER || t->type == TOKEN_WCHAR_LITERAL) {
        node = ast_new(AST_NUMBER);
        strcpy(node->text, t->text);
        advance();
    } else if (match(TOKEN_LBRACKET)) {
        // [ ... ] is not a literal in Come (COIL-100): { } is the one
        // literal syntax for every collection, and [ ] means indexing
        // or an array type. Report it, skip the bracketed list so the
        // rest of the file still parses, and fail in parse_file.
        printf("Error: array literals use { } in Come, not [ ] - e.g. {1, 2, 3} (line %d)\n", t->line);
        g_parse_errors++;
        int depth = 1;
        while (depth > 0 && current()->type != TOKEN_EOF) {
            if (current()->type == TOKEN_LBRACKET) depth++;
            else if (current()->type == TOKEN_RBRACKET) depth--;
            advance();
        }
        node = ast_new(AST_AGGREGATE_INIT);
        strcpy(node->text, "ARRAY");
    } else if (match(TOKEN_LBRACE)) {
        // Map/Struct initializer: { k: v, ... } or { .field = val, ... }
        node = parse_brace_aggregate("MAP");
    } else if (match(TOKEN_LPAREN)) {
        if (is_type_token(current()->type)) {
             // Cast: (int) expr
             char type_name[64];
             strcpy(type_name, current()->text);
             advance();
             // Check array
             while(match(TOKEN_LBRACKET)) {
                 while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
                 expect(TOKEN_RBRACKET);
                 strcat(type_name, "[]");
             }
             expect(TOKEN_RPAREN);
             
             ASTNode* target = parse_primary(); // Cast binds tight
             node = ast_new(AST_CAST);
             ASTNode* tnode = ast_new(AST_IDENTIFIER);
             strcpy(tnode->text, type_name);
             node->children[node->child_count++] = tnode;
             node->children[node->child_count++] = target;
        } else {
             node = parse_expression();
             expect(TOKEN_RPAREN);
        }
    }

    if (!node && is_type_token(t->type) && pos + 1 < tokens.count &&
        tokens.tokens[pos + 1].type == TOKEN_LPAREN) {
        // Functional cast: T(value) - spec 6.8's explicit conversion.
        // (A bare type token in expression position matched no atom
        // branch and used to produce a NULL node.)
        char type_name[64];
        strcpy(type_name, t->text);
        advance(); // type
        advance(); // (
        ASTNode* cast = ast_new(AST_CAST);
        ASTNode* tnode = ast_new(AST_IDENTIFIER);
        strcpy(tnode->text, type_name);
        cast->children[cast->child_count++] = tnode;
        cast->children[cast->child_count++] = parse_expression();
        expect(TOKEN_RPAREN);
        node = cast;
    }

    if (node && node->type == AST_IDENTIFIER && is_type_alias_name(node->text) &&
        current()->type == TOKEN_LPAREN &&
        current()->line == tokens.tokens[pos - 1].line) {
        // Cast through a type alias: tcpport_t(8080) is
        // T(value) with T an alias (spec 6.8, COIL-129).
        ASTNode* cast = ast_new(AST_CAST);
        ASTNode* tnode = ast_new(AST_IDENTIFIER);
        strcpy(tnode->text, node->text);
        cast->children[cast->child_count++] = tnode;
        advance(); // (
        cast->children[cast->child_count++] = parse_expression();
        if (current()->type == TOKEN_COMMA) {
            printf("Error: cast '%s(value)' takes exactly one value at line %d\n",
                   node->text, current()->line);
            g_parse_errors++;
            while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) advance();
        }
        expect(TOKEN_RPAREN);
        free(node);
        node = cast;
    }

    if (!node) return NULL;

    // 2. Postfix Loop (Member access, Call, Array, PostInc/Dec)
    while (1) {
        if (match(TOKEN_DOT)) {
            // Member Access or Method Call
            Token* member = current();
            if (expect(TOKEN_IDENTIFIER)) {
                if (match(TOKEN_LPAREN)) {
                    // Method Call: .ident(...)
                    ASTNode* call = ast_new(AST_METHOD_CALL);
                    call->children[call->child_count++] = node; // Receiver
                    strcpy(call->text, member->text);
                    
                    while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                        call->children[call->child_count++] = parse_expression();
                        if (!match(TOKEN_COMMA)) break;
                    }
                    expect(TOKEN_RPAREN);

                    // Trailing closure: only when the block
                    // opens on the same line as the call's close
                    // paren. A '{' on a later line starts a new
                    // statement (a bare block), not a closure
                    // argument to this call.
                    if (current()->type == TOKEN_LBRACE &&
                        current()->line == tokens.tokens[pos - 1].line) {
                        call->children[call->child_count++] = parse_block();
                    }
                    node = call;
                } else {
                    // Member Access: .ident
                    ASTNode* access = ast_new(AST_MEMBER_ACCESS);
                    access->children[access->child_count++] = node;
                    strcpy(access->text, member->text);
                    node = access;
                }
            }
        } else if (match(TOKEN_LBRACKET)) {
            ASTNode* index = parse_expression();
            expect(TOKEN_RBRACKET);
            ASTNode* access = ast_new(AST_ARRAY_ACCESS);
            access->children[access->child_count++] = node;
            access->children[access->child_count++] = index;
            node = access;
        } else if (current()->type == TOKEN_LPAREN &&
                   current()->line == tokens.tokens[pos - 1].line &&
                   match(TOKEN_LPAREN)) {
            // Function Call: expr(...)   (e.g. func(), arr[0]())
            // Only on the same line as the callee: a '(' starting
            // a later line begins a new statement (statements
            // have no terminators, so this is the boundary).
            
            if (node->type == AST_IDENTIFIER) {
                 ASTNode* call = ast_new(AST_CALL);
                 strcpy(call->text, node->text);
                 free(node);
                 node = call;

                 while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                     node->children[node->child_count++] = parse_expression();
                     if (!match(TOKEN_COMMA)) break;
                 }
                 expect(TOKEN_RPAREN);

                 // Macro alias expansion (spec 5.3): a call to
                 // a macro name is replaced by a clone of the
                 // body with arguments substituted for the
                 // parameters.
                 MacroEntry* macro = find_macro(node->text);
                 if (macro) {
                     if (node->child_count != macro->param_count) {
                         printf("Error: macro %s takes %d argument(s), got %d\n",
                                macro->name, macro->param_count, node->child_count);
                     } else {
                         ASTNode* expanded = ast_clone(macro->body);
                         macro_substitute(expanded, macro, node);
                         node = expanded;
                     }
                 }
            } else if (node->type == AST_MEMBER_ACCESS) {
                 // Convert Member Access + Call -> Method Call (Alias Substitution case)
                 ASTNode* receiver = node->children[0];
                 ASTNode* call = ast_new(AST_METHOD_CALL);
                 strcpy(call->text, node->text); // Method name from member access
                 call->children[call->child_count++] = receiver;
                 free(node);
                 node = call;

                 while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                     node->children[node->child_count++] = parse_expression();
                     if (!match(TOKEN_COMMA)) break;
                 }
                 expect(TOKEN_RPAREN);
            } else {
                 printf("Error: Indirect call not supported on this node type\n");
                 // consume parens to avoid cascade error
                 while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) advance();
                 expect(TOKEN_RPAREN);
            }
        } else if (current()->type == TOKEN_INC &&
                   current()->line == tokens.tokens[pos - 1].line &&
                   match(TOKEN_INC)) {
            // Postfix ++ binds only on the same line: a ++
            // starting the next line is a prefix operator of
            // the next statement (statements end at newlines,
            // spec 15; the call case above has the same rule).
            ASTNode* inc = ast_new(AST_POST_INC);
            inc->children[inc->child_count++] = node;
            node = inc;
        } else if (current()->type == TOKEN_DEC &&
                   current()->line == tokens.tokens[pos - 1].line &&
                   match(TOKEN_DEC)) {
            ASTNode* dec = ast_new(AST_POST_DEC);
            dec->children[dec->child_count++] = node;
            node = dec;
        } else {
            break;
        }
    }
    
    return node;
}

static int get_precedence(TokenType type) {
    // C's relative order (spec 14 lists the bitwise and shift
    // operators; they lexed fine but had no precedence, so
    // the parser rejected them - COIL-128).
    switch (type) {
        case TOKEN_QUESTION: return 1;
        case TOKEN_LOGIC_OR: return 2;
        case TOKEN_LOGIC_AND: return 3;
        case TOKEN_OR: return 4;
        case TOKEN_XOR: return 5;
        case TOKEN_AND: return 6;
        case TOKEN_EQ: case TOKEN_NEQ: return 7;
        case TOKEN_LT: case TOKEN_GT: case TOKEN_LE: case TOKEN_GE: return 8;
        case TOKEN_LSHIFT: case TOKEN_RSHIFT: return 9;
        case TOKEN_PLUS: case TOKEN_MINUS: return 10;
        case TOKEN_STAR: case TOKEN_SLASH: case TOKEN_PERCENT: return 11;
        default: return 0;
    }
}

static ASTNode* parse_expression_prec(int min_prec) {
    ASTNode* lhs = parse_primary();
    if (!lhs) return NULL;

    while (1) {
        Token* t = current();
        int prec = get_precedence(t->type);
        if (prec == 0 || prec < min_prec) break;
        
        // A reference declaration begins a new statement
        // (COIL-136): a line starting with &(struct Name) is
        // never the bitwise-and continuation of the previous
        // line's expression.
        if (t->type == TOKEN_AND && pos >= 1 &&
            t->line > tokens.tokens[pos - 1].line &&
            pos + 2 < tokens.count &&
            tokens.tokens[pos + 1].type == TOKEN_LPAREN &&
            tokens.tokens[pos + 2].type == TOKEN_STRUCT) break;
        
        if (t->type == TOKEN_QUESTION) {
            // Ternary: a ? b : c
            advance(); // consume ?
            ASTNode* true_expr = parse_expression();
            expect(TOKEN_COLON);
            ASTNode* false_expr = parse_expression_prec(prec); // Right associative
            
            ASTNode* ternary = ast_new(AST_TERNARY);
            ternary->children[ternary->child_count++] = lhs;
            ternary->children[ternary->child_count++] = true_expr;
            ternary->children[ternary->child_count++] = false_expr;
            lhs = ternary;
        } else {
            char op_text[32];
            strcpy(op_text, t->text);
            advance(); // consume op

            ASTNode* rhs = parse_expression_prec(prec + 1);
            
            ASTNode* bin = ast_new(AST_BINARY_OP);
            strcpy(bin->text, op_text);
            bin->children[bin->child_count++] = lhs;
            bin->children[bin->child_count++] = rhs;
            lhs = bin;
        }
    }
    return lhs;
}

static ASTNode* parse_expression() {
    return parse_expression_prec(0);
}

// Parse a map type's braces starting at the current '{' token and
// write the normalized type text "map{K, V}" (recursive for a nested
// map value type). COIL-102.
static void parse_map_braces_text(char* out, size_t out_size) {
    char kt[160] = "";
    char vt[160] = "";
    advance(); // '{'
    if (current()->type != TOKEN_COMMA && current()->type != TOKEN_RBRACE) {
        strncpy(kt, current()->text, sizeof(kt) - 1);
        advance();
    }
    if (match(TOKEN_COMMA)) {
        if (strcmp(current()->text, "map") == 0 && pos + 1 < tokens.count &&
            tokens.tokens[pos + 1].type == TOKEN_LBRACE) {
            advance(); // 'map'
            parse_map_braces_text(vt, sizeof(vt));
        } else if (current()->type != TOKEN_RBRACE) {
            strncpy(vt, current()->text, sizeof(vt) - 1);
            advance();
            if (match(TOKEN_LBRACKET)) {
                strcat(vt, "[]");
                expect(TOKEN_RBRACKET);
            }
        }
    }
    expect(TOKEN_RBRACE);
    if (vt[0]) snprintf(out, out_size, "map{%s, %s}", kt, vt);
    else snprintf(out, out_size, "map{%s}", kt);
}

static ASTNode* parse_var_decl() {
    Token* t = current();
    char type_name[256];
    int is_ref_decl = 0;
    if (t->type == TOKEN_AND) {
        // Reference variable (COIL-136): &(struct Name) var = &value.
        // Non-owning; must be initialized (checked in codegen).
        if (g_in_struct_fields) {
            printf("Error: a struct field cannot be a reference - references are parameters and local variables only (line %d)\n", t->line);
            g_parse_errors++;
        }
        advance(); // &
        if (!expect(TOKEN_LPAREN)) return NULL;
        if (current()->type != TOKEN_STRUCT || pos + 1 >= tokens.count ||
            tokens.tokens[pos + 1].type != TOKEN_IDENTIFIER) {
            printf("Error: '&(' must wrap a struct type: &(struct Name) (line %d)\n", t->line);
            g_parse_errors++;
            return NULL;
        }
        advance(); // struct
        snprintf(type_name, sizeof(type_name), "&(struct %.150s)", current()->text);
        advance(); // Name
        if (!expect(TOKEN_RPAREN)) return NULL;
        is_ref_decl = 1;
    } else {
        strcpy(type_name, t->text);
        advance();
    }
    
    // Special handling for struct/union: "struct Type varname" or "union Type varname"
    if ((strcmp(type_name, "struct") == 0 || strcmp(type_name, "union") == 0) && current()->type == TOKEN_IDENTIFIER) {
        // Consume the type name
        strcat(type_name, " ");
        strcat(type_name, current()->text);
        advance();
    }

    // Map type: map{K, V} (COIL-102)
    if (strcmp(type_name, "map") == 0 && current()->type == TOKEN_LBRACE) {
        parse_map_braces_text(type_name, sizeof(type_name));
    }
    
    // Check for array type: int[] x (never for a reference)
    int type_arr_size = is_ref_decl ? 0 : peek_array_size();
    while (!is_ref_decl && match(TOKEN_LBRACKET)) {
         while(current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
         expect(TOKEN_RBRACKET);
         strcat(type_name, "[]");
    }

    if (match(TOKEN_IDENTIFIER)) {
        char var_name[64];
        strcpy(var_name, tokens.tokens[pos-1].text);

        int is_array = 0;
        int name_arr_size = peek_array_size();
        if (match(TOKEN_LBRACKET)) {
            while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
            expect(TOKEN_RBRACKET);
            is_array = 1;
        }
        
         ASTNode* decl = ast_new(AST_VAR_DECL);
         strcpy(decl->text, var_name); // Var name
         
         // Child 0: Initializer expression
         if (tokens.tokens[pos-1].type == TOKEN_ASSIGN) { 
              decl->children[decl->child_count++] = parse_expression();
         } else if (match(TOKEN_ASSIGN)) {
              decl->children[decl->child_count++] = parse_expression();
         } else {
              // No initializer? Uninitialized var.
              ASTNode* dummy = ast_new(AST_NUMBER);
              strcpy(dummy->text, "0"); // Default init
              decl->children[decl->child_count++] = dummy; 
         }

         // Child 1: Type
         ASTNode* type_node = ast_new(AST_IDENTIFIER);
         strcpy(type_node->text, type_name);
         if (is_array) strcat(type_node->text, "[]"); // Mark as array
         type_node->array_size = name_arr_size ? name_arr_size : type_arr_size;
         decl->children[decl->child_count++] = type_node;
         
         if (current()->type == TOKEN_SEMICOLON) advance();
         return decl;
    }
    return NULL;
}

static ASTNode* parse_if_statement() {
    advance(); // Consume IF
    expect(TOKEN_LPAREN);
    ASTNode* cond = parse_expression(); 
    
    Token* next = current();
    if (next->type == TOKEN_EQ || next->type == TOKEN_NEQ || 
        next->type == TOKEN_GT || next->type == TOKEN_LT || 
        next->type == TOKEN_GE || next->type == TOKEN_LE) {
            
            char op[32];
            strcpy(op, next->text);
            advance();
            ASTNode* rhs = parse_expression();
            
            ASTNode* op_node = ast_new(AST_CALL);
            strcpy(op_node->text, op);
            op_node->children[op_node->child_count++] = cond;
            op_node->children[op_node->child_count++] = rhs;
            cond = op_node;
    }
    
    if (current()->type == TOKEN_ASSIGN) {
        // The classic slip: '=' assigns, it does not compare
        // (COIL-129). Recover by consuming the assignment so
        // the rest of the statement still parses; the counted
        // error fails the build.
        printf("Error: '=' does not compare in a condition at line %d - use '==' to compare, or move the assignment out of the condition\n",
               current()->line);
        g_parse_errors++;
        advance(); // '='
        parse_expression(); // discard the assigned value
    }

    if (!match(TOKEN_RPAREN)) {
            printf("Error: expected ')' after the if condition but found '%s' at line %d\n",
                   current()->text, current()->line);
            g_parse_errors++;
    }
    
    ASTNode* node = ast_new(AST_IF);
    node->children[node->child_count++] = cond;
    node->children[node->child_count++] = parse_statement();
    
    if (match(TOKEN_ELSE)) {
        ASTNode* else_node = ast_new(AST_ELSE);
        else_node->children[else_node->child_count++] = parse_statement();
        node->children[node->child_count++] = else_node;
    }
    return node;
}

static ASTNode* parse_switch_statement() {
    advance(); // Consume SWITCH
    expect(TOKEN_LPAREN);
    ASTNode* expr = parse_expression();
    expect(TOKEN_RPAREN);
    
    ASTNode* switch_node = ast_new(AST_SWITCH);
    switch_node->children[switch_node->child_count++] = expr;
    
    expect(TOKEN_LBRACE);
    while(current()->type!=TOKEN_RBRACE && current()->type!=TOKEN_EOF) {
            int start_pos = pos;
            ASTNode* stmt = parse_statement();
            if (stmt) switch_node->children[switch_node->child_count++] = stmt;
            
            if (pos == start_pos) {
                 printf("Error: Unexpected token in switch: %s\n", current()->text);
                 advance();
            }
    }
    expect(TOKEN_RBRACE);
    return switch_node;
}

static ASTNode* parse_case_statement() {
    advance(); // CASE
    ASTNode* case_node = ast_new(AST_CASE);
    case_node->children[case_node->child_count++] = parse_expression();
    expect(TOKEN_COLON);
    while (current()->type != TOKEN_CASE && current()->type != TOKEN_DEFAULT && current()->type != TOKEN_RBRACE && current()->type != TOKEN_EOF) {
            int start_pos = pos;
            ASTNode* s = parse_statement();
            if (s) case_node->children[case_node->child_count++] = s;

            if (pos == start_pos) {
                 printf("Error: Unexpected token in case: %s\n", current()->text);
                 advance();
            }
    }
    return case_node;
}

static ASTNode* parse_default_statement() {
    advance(); // DEFAULT
    expect(TOKEN_COLON);
    ASTNode* def_node = ast_new(AST_DEFAULT);
        while (current()->type != TOKEN_CASE && current()->type != TOKEN_DEFAULT && current()->type != TOKEN_RBRACE && current()->type != TOKEN_EOF) {
            int start_pos = pos;
            ASTNode* s = parse_statement();
            if (s) def_node->children[def_node->child_count++] = s;

            if (pos == start_pos) {
                 printf("Error: Unexpected token in default: %s\n", current()->text);
                 advance();
            }
    }
    return def_node;
}

static ASTNode* parse_while_statement() {
    advance(); // Consume WHILE
    expect(TOKEN_LPAREN);
    ASTNode* cond = parse_expression();
    expect(TOKEN_RPAREN);
    ASTNode* body = parse_block();
    
    ASTNode* node = ast_new(AST_WHILE);
    node->children[node->child_count++] = cond;
    node->children[node->child_count++] = body;
    return node;
}

static ASTNode* parse_do_while_statement() {
    advance(); // Consume DO
    ASTNode* body = parse_block();
    expect(TOKEN_WHILE);
    expect(TOKEN_LPAREN);
    ASTNode* cond = parse_expression();
    expect(TOKEN_RPAREN);
    
    ASTNode* node = ast_new(AST_DO_WHILE);
    node->children[node->child_count++] = body;
    node->children[node->child_count++] = cond;
    return node;
}

static ASTNode* parse_for_statement() {
    advance(); // Consume FOR
    expect(TOKEN_LPAREN);
    ASTNode* node = ast_new(AST_FOR);
    
    // Init (stmt or expr)
    if (current()->type != TOKEN_SEMICOLON) {
            ASTNode* init = parse_statement(); 
            if (init) node->children[node->child_count++] = init;
    } else {
            node->children[node->child_count++] = NULL;
    }
    if (current()->type == TOKEN_SEMICOLON) advance(); 

    // Condition
    if (current()->type != TOKEN_SEMICOLON) {
            ASTNode* cond = parse_expression();
            node->children[node->child_count++] = cond;
    } else {
            node->children[node->child_count++] = NULL;
    }
    if (current()->type == TOKEN_SEMICOLON) advance(); 
    
    // Iteration
    if (current()->type != TOKEN_RPAREN) {
            ASTNode* iter = parse_expression(); 
            node->children[node->child_count++] = iter;
    } else {
            node->children[node->child_count++] = NULL;
    }
    expect(TOKEN_RPAREN);
    
    ASTNode* body = parse_statement(); 
    node->children[node->child_count++] = body;
    
    return node;
}

static ASTNode* parse_return_statement() {
    advance(); // Consume RETURN
    ASTNode* node = ast_new(AST_RETURN);
    if (current()->type != TOKEN_RBRACE && current()->type != TOKEN_SEMICOLON) { 
            ASTNode* expr = parse_expression();
            if (expr) {
                node->children[node->child_count++] = expr;
                while(match(TOKEN_COMMA)) {
                    node->children[node->child_count++] = parse_expression();
                }
            }
    }
    if (current()->type == TOKEN_SEMICOLON) advance();
    return node;
}

static ASTNode* parse_expression_statement() {
    ASTNode* node = parse_expression();
    // Check for assignment after parsing expression (e.g. member/array access LHS)
    // This duplicates logic inside parse_identifier_statement partially but handles non-identifier starts (e.g. *ptr = val)
    if (pos < tokens.count &&
        (tokens.tokens[pos].type == TOKEN_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_PLUS_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_MINUS_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_STAR_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_SLASH_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_MOD_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_AND_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_OR_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_XOR_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_LSHIFT_ASSIGN ||
         tokens.tokens[pos].type == TOKEN_RSHIFT_ASSIGN)) {

           ASTNode* assign = ast_new(AST_ASSIGN);
           strcpy(assign->text, tokens.tokens[pos].text);
           match(tokens.tokens[pos].type); // consume op
           
           assign->children[assign->child_count++] = node;
           assign->children[assign->child_count++] = parse_expression();
           
           if (current()->type == TOKEN_SEMICOLON) advance();
           return assign;
    }

    if (current()->type == TOKEN_SEMICOLON) advance();
    return node;
}

static ASTNode* parse_identifier_statement() {
    Token* t = current();
    
    // Check for assignments FIRST (before type declarations)
    // Check lookahead for assignment operators
    if (pos + 1 < tokens.count &&
        (tokens.tokens[pos+1].type == TOKEN_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_PLUS_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_MINUS_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_STAR_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_SLASH_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_MOD_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_AND_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_OR_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_XOR_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_LSHIFT_ASSIGN ||
         tokens.tokens[pos+1].type == TOKEN_RSHIFT_ASSIGN)) {
          
           ASTNode* assign = ast_new(AST_ASSIGN);
           strcpy(assign->text, tokens.tokens[pos+1].text); // The operator
           
           ASTNode* lhs = ast_new(AST_IDENTIFIER);
           strcpy(lhs->text, t->text);
           assign->children[assign->child_count++] = lhs;
           
           advance(); // ident
           advance(); // op
           assign->children[assign->child_count++] = parse_expression();
           
           if (current()->type == TOKEN_SEMICOLON) advance();
           return assign;
    }
    
    // Then check for custom type declaration: MyType x ...
    // Lookahead 1
    if (pos + 1 < tokens.count && tokens.tokens[pos+1].type == TOKEN_IDENTIFIER) {
         // Treat as declaration
         char type_name[64];
         strcpy(type_name, t->text);
         advance(); // consume type
         
         char var_name[64];
         strcpy(var_name, tokens.tokens[pos].text);
         advance(); // consume var name
         
         // Check array
         int is_array = 0;
         int arr_size = peek_array_size();
         if (match(TOKEN_LBRACKET)) {
             while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
             expect(TOKEN_RBRACKET);
             is_array = 1;
         }

         ASTNode* decl = ast_new(AST_VAR_DECL);
         strcpy(decl->text, var_name);

         // Init
         if (match(TOKEN_ASSIGN)) {
             decl->children[decl->child_count++] = parse_expression();
         } else {
             // Default init 0
             ASTNode* dummy = ast_new(AST_NUMBER);
             strcpy(dummy->text, "0");
             decl->children[decl->child_count++] = dummy;
         }

         // Type
         ASTNode* type_node = ast_new(AST_IDENTIFIER);
         strcpy(type_node->text, type_name);
         if (is_array) strcat(type_node->text, "[]");
         type_node->array_size = arr_size;
         decl->children[decl->child_count++] = type_node;
         if (current()->type == TOKEN_SEMICOLON) advance();
         return decl;
    } 
    
    // Fallback to expression or complex assignment (e.g. arr[i] = val)
    return parse_expression_statement();
}

// Struct layout modifiers (COIL-104): 'packed' and 'align(n)' may
// precede a struct declaration in either order. The lookahead keeps
// ordinary identifiers named packed/align usable as variables.
static int align_pattern_at(int p) {
    // tokens[p] == "align", then ( NUMBER ) then 'struct' or 'packed'
    if (p + 4 >= tokens.count) return 0;
    if (tokens.tokens[p + 1].type != TOKEN_LPAREN) return 0;
    if (tokens.tokens[p + 2].type != TOKEN_NUMBER) return 0;
    if (tokens.tokens[p + 3].type != TOKEN_RPAREN) return 0;
    if (tokens.tokens[p + 4].type == TOKEN_STRUCT) return 1;
    return tokens.tokens[p + 4].type == TOKEN_IDENTIFIER &&
           strcmp(tokens.tokens[p + 4].text, "packed") == 0 &&
           p + 5 < tokens.count && tokens.tokens[p + 5].type == TOKEN_STRUCT;
}

static int at_struct_modifier() {
    Token* t = current();
    if (t->type != TOKEN_IDENTIFIER) return 0;
    if (strcmp(t->text, "packed") == 0) {
        if (pos + 1 < tokens.count && tokens.tokens[pos + 1].type == TOKEN_STRUCT) return 1;
        return pos + 1 < tokens.count && tokens.tokens[pos + 1].type == TOKEN_IDENTIFIER &&
               strcmp(tokens.tokens[pos + 1].text, "align") == 0 && align_pattern_at(pos + 1);
    }
    if (strcmp(t->text, "align") == 0) return align_pattern_at(pos);
    return 0;
}

// Consume the modifier sequence; current() ends at 'struct'.
// Returns 1 when 'packed' was present; *out_align gets the align(n).
static int parse_struct_modifiers(int* out_align) {
    int packed = 0;
    *out_align = 0;
    while (current()->type == TOKEN_IDENTIFIER &&
           (strcmp(current()->text, "packed") == 0 || strcmp(current()->text, "align") == 0)) {
        if (strcmp(current()->text, "packed") == 0) {
            packed = 1;
            advance();
        } else {
            advance(); // align
            if (match(TOKEN_LPAREN)) {
                if (current()->type == TOKEN_NUMBER) {
                    *out_align = atoi(current()->text);
                    advance();
                }
                expect(TOKEN_RPAREN);
            }
        }
    }
    return packed;
}

static ASTNode* parse_struct_statement() {
    advance(); // struct
    if (expect(TOKEN_IDENTIFIER)) {
        char struct_name[64];
        strcpy(struct_name, tokens.tokens[pos-1].text);
        
        if (match(TOKEN_LBRACE)) {
            ASTNode* node = ast_new(AST_STRUCT_DECL);
            strcpy(node->text, struct_name);
            
            while (current()->type != TOKEN_RBRACE && current()->type != TOKEN_EOF) {
                 int start_pos = pos;
                 g_in_struct_fields = 1;
                 ASTNode* field = parse_statement();
                 g_in_struct_fields = 0;
                 if (field) node->children[node->child_count++] = field;
                 
                 if (pos == start_pos) {
                     printf("Error: Unexpected token in struct statement: %s\n", current()->text);
                     advance();
                 }
            }
            expect(TOKEN_RBRACE);
            return node;
        }
    }
    return NULL; 
}

static ASTNode* parse_method_statement() {
    advance(); // method
    if (expect(TOKEN_IDENTIFIER)) {
        char name[64];
        strcpy(name, tokens.tokens[pos-1].text);
        expect(TOKEN_LPAREN);
        while(current()->type!=TOKEN_RPAREN && current()->type!=TOKEN_EOF) advance();
        expect(TOKEN_RPAREN);
        ASTNode* node = ast_new(AST_FUNCTION);
        strcpy(node->text, name);
        return node; 
    }
    return NULL;
}

static ASTNode* parse_alias_statement() {
    advance(); // alias
    
    // Single alias: alias Name = Expression/Type
    if (expect(TOKEN_IDENTIFIER)) {
         char alias_name[64];
         strcpy(alias_name, tokens.tokens[pos-1].text);
         if (match(TOKEN_ASSIGN)) {
             // Parse the target as an expression (handles std.out.printf)
             // We use parse_primary to catch identifiers/member access
             // We might need parse_expression? 
             // std.out.printf is a Member Access chain.
             // parse_expression handles that.
             ASTNode* target = parse_expression();
             
             if (target) {
                 // Register for substitution
                 register_alias(alias_name, target);
                 
                 // Return NULL or a dummy node?
                 // If we return NULL, parse_statement might return NULL?
                 // But parse_statement swallows it? 
                 // If we return a node, codegen will try to generate it.
                 // We want strictly compiler-time alias for now?
                 // Existing code generated AST_TYPE_ALIAS.
                 // For method alias, we don't want runtime code.
                 // Let's return NULL so it disappears from AST?
                 // parse_statement caller handles NULL? check below.
                 // parse_statement calls it. If returns NULL...
                 // parse_block: if (stmt) block->children...
                 // So returning NULL is safe and correct for compile-time directive.
                 // BUT we need to be careful about semicolon?
                 
                 // Consume optional semicolon
                 if (current()->type == TOKEN_SEMICOLON) advance();
                 
                 return NULL; 
             }
         }
    }
    return NULL;
}

static ASTNode* parse_statement() {
    Token* t = current();

    // Channel creation (COIL-123, spec 9.2):
    //   (a, b) = colink()
    // declares two link variables holding the channel's ends.
    if (t->type == TOKEN_LPAREN && pos + 5 < tokens.count &&
        tokens.tokens[pos + 1].type == TOKEN_IDENTIFIER &&
        tokens.tokens[pos + 2].type == TOKEN_COMMA &&
        tokens.tokens[pos + 3].type == TOKEN_IDENTIFIER &&
        tokens.tokens[pos + 4].type == TOKEN_RPAREN &&
        tokens.tokens[pos + 5].type == TOKEN_ASSIGN) {
        ASTNode* node = ast_new(AST_COLINK_ASSIGN);
        advance(); // (
        ASTNode* n1 = ast_new(AST_IDENTIFIER);
        strcpy(n1->text, current()->text);
        advance();
        advance(); // ,
        ASTNode* n2 = ast_new(AST_IDENTIFIER);
        strcpy(n2->text, current()->text);
        advance();
        expect(TOKEN_RPAREN);
        expect(TOKEN_ASSIGN);
        node->children[node->child_count++] = n1;
        node->children[node->child_count++] = n2;
        node->children[node->child_count++] = parse_expression();
        if (current()->type == TOKEN_SEMICOLON) advance();
        return node;
    }

    // Struct definition with layout modifiers (COIL-104)
    if (at_struct_modifier()) {
        int m_align = 0;
        int m_packed = parse_struct_modifiers(&m_align);
        if (current()->type == TOKEN_STRUCT) {
            ASTNode* sn = parse_struct_statement();
            if (sn) {
                sn->layout_packed = m_packed;
                sn->layout_align = m_align;
            }
            return sn;
        }
    }

    // Check for struct definition explicitly
    if (t->type == TOKEN_STRUCT && pos+2 < tokens.count &&
        tokens.tokens[pos+1].type == TOKEN_IDENTIFIER &&
        tokens.tokens[pos+2].type == TOKEN_LBRACE) {
          return parse_struct_statement();
    }

    if (t->type == TOKEN_AND && pos + 2 < tokens.count &&
        tokens.tokens[pos + 1].type == TOKEN_LPAREN &&
        tokens.tokens[pos + 2].type == TOKEN_STRUCT) {
        // Reference variable declaration (COIL-136).
        ASTNode* decl = parse_var_decl();
        if (decl) return decl;
    }

    if (is_type_token(t->type)) {
        ASTNode* decl = parse_var_decl();
        if (decl) return decl; 
    }
    
    switch (t->type) {
        case TOKEN_IDENTIFIER: return parse_identifier_statement();
        case TOKEN_IF: return parse_if_statement();
        case TOKEN_SWITCH: return parse_switch_statement();
        case TOKEN_CASE: return parse_case_statement();
        case TOKEN_DEFAULT: return parse_default_statement();
        case TOKEN_WHILE: return parse_while_statement();
        case TOKEN_DO: return parse_do_while_statement();
        case TOKEN_FOR: return parse_for_statement();
        case TOKEN_RETURN: return parse_return_statement();
        case TOKEN_SPAWN: {
            // spawn name(args) — a statement (spec 9.1)
            advance();
            ASTNode* node = ast_new(AST_SPAWN);
            if (expect(TOKEN_IDENTIFIER)) {
                strcpy(node->text, tokens.tokens[pos - 1].text);
            }
            expect(TOKEN_LPAREN);
            while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                node->children[node->child_count++] = parse_expression();
                if (!match(TOKEN_COMMA)) break;
            }
            expect(TOKEN_RPAREN);
            if (current()->type == TOKEN_SEMICOLON) advance();
            return node;
        }
        case TOKEN_YIELD: {
            advance();
            ASTNode* node = ast_new(AST_YIELD);
            if (current()->type == TOKEN_SEMICOLON) advance();
            return node;
        }
        case TOKEN_LBRACE: return parse_block();
        case TOKEN_METHOD: return parse_method_statement();
        case TOKEN_ALIAS: return parse_alias_statement();
        case TOKEN_BREAK: {
            advance();
            ASTNode* node = ast_new(AST_BREAK);
            if (current()->type == TOKEN_SEMICOLON) advance();
            return node;
        }
        case TOKEN_CONTINUE: {
            advance();
            ASTNode* node = ast_new(AST_CONTINUE);
            if (current()->type == TOKEN_SEMICOLON) advance();
            return node;
        }
        case TOKEN_FALLTHROUGH: advance(); return ast_new(AST_FALLTHROUGH);
        default: return parse_expression_statement();
    }
}

static ASTNode* parse_block() {
    expect(TOKEN_LBRACE);
    ASTNode* block = ast_new(AST_BLOCK);
    while (current()->type != TOKEN_RBRACE && current()->type != TOKEN_EOF) {
        int start_pos = pos;
        ASTNode* stmt = parse_statement();
        if (stmt) block->children[block->child_count++] = stmt;

        if (pos == start_pos) {
             printf("Error: Unexpected token in block: %s\n", current()->text);
             advance();
        }
    }
    expect(TOKEN_RBRACE);
    return block;
}

// extern "C" <ret> <name>(<params>) - a raw C function declaration
// (COIL-111, spec 13). Produces a body-less AST_FUNCTION marked
// is_extern; codegen emits the unmangled C prototype and rejects
// the declaration outside system modules. Parameter parsing mirrors
// the ordinary parameter shapes (types, arrays, fn declarators).
static void parse_extern(ASTNode* program) {
    advance(); // extern
    if (current()->type != TOKEN_STRING_LITERAL || strcmp(current()->text, "\"C\"") != 0) {
        printf("Error: extern must be written extern \"C\" (line %d)\n", current()->line);
        g_parse_errors++;
        return;
    }
    advance(); // "C"

    char ret_type[256];
    if (current()->type == TOKEN_STRUCT) {
        advance();
        snprintf(ret_type, sizeof(ret_type), "struct %.248s", current()->text);
        advance();
    } else {
        snprintf(ret_type, sizeof(ret_type), "%.255s", current()->text);
        advance();
    }
    if (match(TOKEN_LBRACKET)) {
        while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
        expect(TOKEN_RBRACKET);
        strcat(ret_type, "[]");
    }
    if (current()->type != TOKEN_IDENTIFIER) {
        printf("Error: extern \"C\" declaration needs a function name (line %d)\n", current()->line);
        g_parse_errors++;
        return;
    }
    ASTNode* fn = ast_new(AST_FUNCTION);
    fn->is_extern = 1;
    strcpy(fn->text, current()->text);
    advance();
    ASTNode* ret = ast_new(AST_IDENTIFIER);
    strcpy(ret->text, ret_type);
    fn->children[fn->child_count++] = ret;

    expect(TOKEN_LPAREN);
    while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
        if (current()->type == TOKEN_COMMA) { advance(); continue; }
        if (current()->type == TOKEN_CONST) advance();
        char pt[256];
        if (current()->type == TOKEN_STRUCT) {
            advance();
            snprintf(pt, sizeof(pt), "struct %.248s", current()->text);
            advance();
        } else {
            snprintf(pt, sizeof(pt), "%.255s", current()->text);
            advance();
        }
        if (match(TOKEN_LBRACKET)) {
            while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
            expect(TOKEN_RBRACKET);
            strcat(pt, "[]");
        }
        ASTNode* arg = ast_new(AST_VAR_DECL);
        if (current()->type == TOKEN_IDENTIFIER) {
            strcpy(arg->text, current()->text);
            advance();
        }
        if (match(TOKEN_LBRACKET)) {
            while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
            expect(TOKEN_RBRACKET);
            strcat(pt, "[]");
        }
        // Function-declarator parameter (qsort's comparator shape)
        if (current()->type == TOKEN_LPAREN) {
            char fn_type[512];
            snprintf(fn_type, sizeof(fn_type), "fn:%s(", pt);
            advance(); // (
            int first = 1;
            while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                if (current()->type == TOKEN_COMMA) { advance(); continue; }
                if (current()->type == TOKEN_CONST) advance();
                char ipt[160];
                snprintf(ipt, sizeof(ipt), "%.150s", current()->text);
                advance();
                if (current()->type == TOKEN_IDENTIFIER) advance();
                if (!first) strcat(fn_type, ",");
                strcat(fn_type, ipt);
                first = 0;
            }
            expect(TOKEN_RPAREN);
            strcat(fn_type, ")");
            strcpy(pt, fn_type);
        }
        ASTNode* at = ast_new(AST_IDENTIFIER);
        strcpy(at->text, pt);
        arg->children[arg->child_count++] = NULL; // no init
        arg->children[arg->child_count++] = at;
        fn->children[fn->child_count++] = arg;
    }
    expect(TOKEN_RPAREN);
    match(TOKEN_SEMICOLON);
    program->children[program->child_count++] = fn;
}

static void parse_import(ASTNode* program) {
    advance(); // import
    if (match(TOKEN_LPAREN)) {
        // import ( std, string )
        while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
            if (current()->type == TOKEN_IDENTIFIER || current()->type == TOKEN_STRING_LITERAL || current()->type == TOKEN_STRING) {
                ASTNode* imp = ast_new(AST_IMPORT);
                strcpy(imp->text, current()->text);
                program->children[program->child_count++] = imp;
                advance();
                match(TOKEN_COMMA);
            } else {
                advance();
            }
        }
        expect(TOKEN_RPAREN);
    } else {
        // import std
        if (current()->type == TOKEN_IDENTIFIER || current()->type == TOKEN_STRING_LITERAL || current()->type == TOKEN_STRING) {
            ASTNode* imp = ast_new(AST_IMPORT);
            strcpy(imp->text, current()->text);
            program->children[program->child_count++] = imp;
            advance();
            // `import X as ...` forces system-module import
            // (spec 3.2): local resolution is skipped. The
            // flag rides on the import node (is_extern slot).
            if (current()->type == TOKEN_IDENTIFIER && strcmp(current()->text, "as") == 0) {
                imp->is_extern = 1;
                advance(); // as
                if (current()->type == TOKEN_IDENTIFIER) advance(); // depot qualifier
            }
            while(match(TOKEN_COMMA)) {
                 if (current()->type == TOKEN_IDENTIFIER || current()->type == TOKEN_STRING_LITERAL || current()->type == TOKEN_STRING) {
                     ASTNode* imp2 = ast_new(AST_IMPORT);
                     strcpy(imp2->text, current()->text);
                     program->children[program->child_count++] = imp2;
                     advance();
                 }
            }
        }
    }
}

static void parse_export(ASTNode* program) {
    advance();
    // Record the exported names in an AST_EXPORT node so the
    // driver can enforce visibility (spec 3.4, COIL-127).
    // The entries themselves still parse as declarations,
    // exactly as before.
    ASTNode* exp = ast_new(AST_EXPORT);
    program->children[program->child_count++] = exp;
    if (match(TOKEN_LPAREN)) {
        while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
            int start_pos = pos;
            if (current()->type == TOKEN_COMMA) {
                advance();
                continue;
            }
            // A plain name entry (PI, Rect) is just a name:
            // consume and record it here. (It used to fall
            // through parse_top_level_decl's silent token
            // eat; that fallback now reports an error -
            // COIL-128.)
            if (current()->type == TOKEN_IDENTIFIER &&
                (tokens.tokens[pos + 1].type == TOKEN_COMMA ||
                 tokens.tokens[pos + 1].type == TOKEN_RPAREN)) {
                ASTNode* name_node = ast_new(AST_IDENTIFIER);
                strcpy(name_node->text, current()->text);
                exp->children[exp->child_count++] = name_node;
                advance();
                continue;
            }
            // A typed declaration entry (FILE in) names its
            // variable: record the second identifier.
            if (current()->type == TOKEN_IDENTIFIER &&
                tokens.tokens[pos + 1].type == TOKEN_IDENTIFIER &&
                (tokens.tokens[pos + 2].type == TOKEN_COMMA ||
                 tokens.tokens[pos + 2].type == TOKEN_RPAREN)) {
                ASTNode* name_node = ast_new(AST_IDENTIFIER);
                strcpy(name_node->text, tokens.tokens[pos + 1].text);
                exp->children[exp->child_count++] = name_node;
                advance();
                advance();
                continue;
            }
            // A signature entry whose return type is a named
            // type (FILE tmpfile()) does not fit
            // parse_top_level_decl's entry shapes either.
            // These entries are prototypes (skipped for the
            // AST, as before): consume through the matching
            // ')' and record the function's name.
            if (current()->type == TOKEN_IDENTIFIER &&
                tokens.tokens[pos + 1].type == TOKEN_IDENTIFIER &&
                tokens.tokens[pos + 2].type == TOKEN_LPAREN) {
                ASTNode* name_node = ast_new(AST_IDENTIFIER);
                strcpy(name_node->text, tokens.tokens[pos + 1].text);
                exp->children[exp->child_count++] = name_node;
                advance();
                advance();
                int depth = 0;
                while (pos < tokens.count) {
                    if (current()->type == TOKEN_LPAREN) depth++;
                    if (current()->type == TOKEN_RPAREN && --depth == 0) {
                        advance();
                        break;
                    }
                    if (current()->type == TOKEN_EOF) break;
                    advance();
                }
                continue;
            }
            // A signature entry (int add(int, int)) names the
            // identifier before its '('. Watch the tokens,
            // not the AST.
            int entry_start = pos;
            parse_top_level_decl(program);
            const char* entry_name = NULL;
            char dotted[256];
            if (entry_start < tokens.count &&
                tokens.tokens[entry_start].type == TOKEN_IDENTIFIER) {
                entry_name = tokens.tokens[entry_start].text;
            } else {
                for (int i = entry_start; i < pos && i < tokens.count; i++) {
                    if (tokens.tokens[i].type == TOKEN_LPAREN && i > entry_start) {
                        snprintf(dotted, sizeof(dotted), "%.150s", tokens.tokens[i - 1].text);
                        char* dot = strrchr(dotted, '.');
                        entry_name = dot ? dot + 1 : dotted;
                        break;
                    }
                }
            }
            if (entry_name) {
                ASTNode* name_node = ast_new(AST_IDENTIFIER);
                strcpy(name_node->text, entry_name);
                exp->children[exp->child_count++] = name_node;
            }

            if (pos == start_pos) {
                 printf("Error: Unexpected token in export: %s\n", current()->text);
                 advance();
            }
        }
        expect(TOKEN_RPAREN);
    } else {
        ASTNode* name_node = ast_new(AST_IDENTIFIER);
        strcpy(name_node->text, current()->text);
        exp->children[exp->child_count++] = name_node;
        advance(); // export symbol
    }
}

static void parse_const(ASTNode* program) {
     // const ( ... ) OR const X = ...
     advance();
     if (match(TOKEN_LPAREN)) {
         ASTNode* group = ast_new(AST_CONST_GROUP);
         while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
             // Ident [= val] [, or newline]
             if (current()->type == TOKEN_IDENTIFIER) {
                 ASTNode* node = ast_new(AST_CONST_DECL);
                 strcpy(node->text, current()->text);
                 advance();
                 
                 // Check for = val
                 if (match(TOKEN_ASSIGN)) {
                     // Check enum
                     if (match(TOKEN_ENUM)) {
                         ASTNode* en = ast_new(AST_ENUM_DECL);
                         if (match(TOKEN_LPAREN)) {
                              // enum(start)
                              en->children[en->child_count++] = parse_expression();
                              expect(TOKEN_RPAREN);
                         }
                         node->children[node->child_count++] = en;
                     } else {
                         node->children[node->child_count++] = parse_expression();
                     }
                 } else {
                     // Implicit enum or just declaration?
                     // Assume enum decl in const block if no value 
                     ASTNode* en = ast_new(AST_ENUM_DECL);
                     node->children[node->child_count++] = en;
                 }
                 group->children[group->child_count++] = node;
                 match(TOKEN_COMMA);
             } else {
                 advance(); // skip unknown in block
             }
         }
         expect(TOKEN_RPAREN);
         program->children[program->child_count++] = group;
     } else {
         // const X = ...
         if (expect(TOKEN_IDENTIFIER)) {
             ASTNode* node = ast_new(AST_CONST_DECL);
             strcpy(node->text, tokens.tokens[pos-1].text);
             if (match(TOKEN_ASSIGN)) {
                 node->children[node->child_count++] = parse_expression();
             }
             program->children[program->child_count++] = node;
         }
     }
}

static void parse_union(ASTNode* program) {
    advance();
    if (expect(TOKEN_IDENTIFIER)) {
        ASTNode* node = ast_new(AST_UNION_DECL);
        strcpy(node->text, tokens.tokens[pos-1].text);
        expect(TOKEN_LBRACE);
        while(current()->type!=TOKEN_RBRACE && current()->type!=TOKEN_EOF) {
             int start_pos = pos;
             g_in_struct_fields = 1;
             ASTNode* field = parse_statement(); // Reusing var parsing
             g_in_struct_fields = 0;
             if (field) node->children[node->child_count++] = field;

             if (pos == start_pos) {
                 printf("Error: Unexpected token in union: %s\n", current()->text);
                 advance();
             }
        }
        expect(TOKEN_RBRACE);
        program->children[program->child_count++] = node;
    }
}

static void parse_struct(ASTNode* program) {
    advance();
    if (expect(TOKEN_IDENTIFIER)) {
        ASTNode* node = ast_new(AST_STRUCT_DECL);
        strcpy(node->text, tokens.tokens[pos-1].text);
        
        if (match(TOKEN_LBRACE)) {
            while(current()->type!=TOKEN_RBRACE && current()->type!=TOKEN_EOF) {
                 int start_pos = pos;
                 // Check for method decl inside struct: method name() OR Type name()
                 int is_method = 0;
                 if (match(TOKEN_METHOD)) {
                     is_method = 1;
                 } else {
                     // Lookahead for Type name(
                     // We can peek.
                     // If current is Type, Next is Ident, NextNext is LPAREN -> Method.
                     if (is_type_token(current()->type)) {
                         if (tokens.tokens[pos+1].type == TOKEN_IDENTIFIER && 
                             tokens.tokens[pos+2].type == TOKEN_LPAREN) {
                             is_method = 1;
                             // Don't consume yet, handled inside if
                         }
                     }
                 }

                 if (is_method) {
                     // method ident() OR Type ident()
                     char ret_type[64] = "void"; 
                     if (current()->type != TOKEN_METHOD) {
                         // It was Type ident(...)
                         strcpy(ret_type, current()->text);
                         advance(); // consume type
                     }

                     if (expect(TOKEN_IDENTIFIER)) {
                         // Method name
                         char method_name[64];
                         strcpy(method_name, tokens.tokens[pos-1].text);
                         
                         if (match(TOKEN_LPAREN)) {
                             // Consume tokens until matching RPAREN
                             int balance = 1;
                             while(balance > 0 && current()->type != TOKEN_EOF) {
                                 if (current()->type == TOKEN_LPAREN) balance++;
                                 else if (current()->type == TOKEN_RPAREN) balance--;
                                 advance();
                             }
                             // TODO: Store method signature in AST for full support
                        }
                     }
                 } else {
                     g_in_struct_fields = 1;
                     ASTNode* field = parse_statement();
                     g_in_struct_fields = 0;
                     if (field) node->children[node->child_count++] = field;
                 }

                 if (pos == start_pos) {
                     printf("Error: Unexpected token in struct: %s\n", current()->text);
                     advance();
                 }
            }
            expect(TOKEN_RBRACE);
            // Handle trailing optional semicolon
            match(TOKEN_SEMICOLON);
            program->children[program->child_count++] = node;
         }
    }
}

static int is_type_token(TokenType type) {
    return (type == TOKEN_INT || type == TOKEN_VOID || type == TOKEN_STRING || 
            type == TOKEN_BOOL || type == TOKEN_FLOAT || type == TOKEN_DOUBLE ||
            type == TOKEN_BYTE || type == TOKEN_UBYTE ||
            type == TOKEN_SHORT || type == TOKEN_USHORT ||
            type == TOKEN_UINT ||
            type == TOKEN_LONG || type == TOKEN_ULONG ||
            type == TOKEN_WCHAR || type == TOKEN_MAP || type == TOKEN_VAR ||
            type == TOKEN_LINK ||
            type == TOKEN_STRUCT || type == TOKEN_UNION);
}

static void parse_single_alias(ASTNode* program) {
    if (match(TOKEN_IDENTIFIER) || match(TOKEN_STRING) || match(TOKEN_MAP)) {
        char name[256];
        strcpy(name, tokens.tokens[pos-1].text);

        // Handle Hierarchical Alias: alias string.len = ...
        while (match(TOKEN_DOT)) {
            strcat(name, ".");
            if (current()->type == TOKEN_IDENTIFIER || current()->type == TOKEN_STRING || current()->type == TOKEN_MAP) {
                strcat(name, current()->text);
                advance();
            }
        }

        if (match(TOKEN_LPAREN)) {
            // Macro alias: alias SQUARE(x) = ((x) * (x))
            // (spec 5.3). Capture the parameter names and the
            // body as a template for call-site expansion.
            char params[8][64];
            int pc = 0;
            while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                if (current()->type == TOKEN_COMMA) { advance(); continue; }
                if (pc < 8) snprintf(params[pc++], 64, "%.*s", 63, current()->text);
                advance();
            }
            expect(TOKEN_RPAREN);
            if (match(TOKEN_ASSIGN)) {
                ASTNode* body = parse_expression();
                if (macro_count < MAX_MACROS) {
                    MacroEntry* m = &macro_table[macro_count++];
                    strcpy(m->name, name);
                    m->param_count = pc;
                    for (int i = 0; i < pc; i++) strcpy(m->params[i], params[i]);
                    m->body = body;
                }
            }
        }
        else if (match(TOKEN_ASSIGN)) {
             // alias X = Y
             // Check if Y is type
             if (is_type_token(current()->type) || current()->type == TOKEN_STRUCT || current()->type == TOKEN_UNION) {
                  // Type Alias
                  ASTNode* node = ast_new(AST_TYPE_ALIAS);
                  strcpy(node->text, name);
                  register_type_alias(name);

                  ASTNode* typeNode = ast_new(AST_IDENTIFIER);
                  
                  if (current()->type == TOKEN_STRUCT) {
                      advance();
                      char t[256];
                      snprintf(t, sizeof(t), "struct %.150s", current()->text);
                      strcpy(typeNode->text, t);
                      advance();
                  } else if (current()->type == TOKEN_UNION) {
                      advance();
                      char t[256];
                      snprintf(t, sizeof(t), "union %.150s", current()->text);
                      strcpy(typeNode->text, t);
                      advance();
                  } else {
                      strcpy(typeNode->text, current()->text);
                      advance();
                  }
                  
                  node->children[0] = typeNode;
                  node->child_count = 1;
                  program->children[program->child_count++] = node;
             } else {
                  // Constant/Expression Alias -> Substitution
                  // alias name = expr
                  // Parse expression and store in alias table
                  ASTNode* expr = parse_expression();
                  if (expr) {
                      register_alias(name, expr);
                  }
                  if (current()->type == TOKEN_SEMICOLON) advance();
             }
        }
    }
}

// Read one type spelling for the paired grouped alias:
// [struct|union] name, with an optional [] suffix. Writes
// the canonical text ("struct Point", "int[]", ...).
static int parse_alias_type_text(char* out, size_t outsz) {
    out[0] = 0;
    if (current()->type == TOKEN_STRUCT || current()->type == TOKEN_UNION) {
        snprintf(out, outsz, "%s ", current()->text);
        advance();
    }
    if (current()->type == TOKEN_EOF) return 0;
    strncat(out, current()->text, outsz - strlen(out) - 1);
    advance();
    if (match(TOKEN_LBRACKET)) {
        while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
        expect(TOKEN_RBRACKET);
        strncat(out, "[]", outsz - strlen(out) - 1);
    }
    return 1;
}

static void parse_alias(ASTNode* program) {
    advance(); // alias
    if (current()->type == TOKEN_LPAREN) {
        // Spec 5.1 paired grouped form:
        //   alias (a, b) = (T1, T2)
        // Scan the name list first; if a '=' and a second
        // list follow, it is the paired form. Otherwise fall
        // back to a list of single aliases.
        int save = pos;
        char names[16][64];
        int name_is_ident[16];
        int name_count = 0;
        int paired = 0;
        advance(); // '('
        while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
            if (current()->type == TOKEN_COMMA) { advance(); continue; }
            if (name_count < 16) {
                // The lexer rewrites built-in alias spellings
                // (i8, i16, ...) to their canonical type text,
                // so a name may arrive as a type token: record
                // that; it can only re-declare itself.
                name_is_ident[name_count] = current()->type == TOKEN_IDENTIFIER;
                snprintf(names[name_count++], 64, "%.*s", 63, current()->text);
            }
            advance();
        }
        if (match(TOKEN_RPAREN) && current()->type == TOKEN_ASSIGN) {
            advance(); // '='
            if (current()->type == TOKEN_LPAREN) paired = 1;
        }
        if (paired) {
            advance(); // '('
            char types[16][300];
            int type_count = 0;
            while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                if (current()->type == TOKEN_COMMA) { advance(); continue; }
                if (type_count < 16 && parse_alias_type_text(types[type_count], 300)) type_count++;
                else advance();
            }
            expect(TOKEN_RPAREN);
            if (type_count != name_count) {
                printf("Error: grouped alias pairs %d names with %d types\n", name_count, type_count);
                return;
            }
            for (int i = 0; i < name_count; i++) {
                if (!name_is_ident[i]) {
                    // A built-in alias name (arrived as its
                    // canonical type token): re-declaring it as
                    // the same type is a no-op; anything else
                    // would redefine the built-in.
                    if (strcmp(names[i], types[i]) != 0) {
                        printf("Error: grouped alias cannot redefine built-in alias '%s' as '%s'\n", names[i], types[i]);
                    }
                    continue;
                }
                ASTNode* node = ast_new(AST_TYPE_ALIAS);
                strcpy(node->text, names[i]);
                register_type_alias(names[i]);
                ASTNode* typeNode = ast_new(AST_IDENTIFIER);
                strcpy(typeNode->text, types[i]);
                node->children[0] = typeNode;
                node->child_count = 1;
                program->children[program->child_count++] = node;
            }
            return;
        }
        pos = save; // not the paired form: reparse as singles
    }
    if (match(TOKEN_LPAREN)) {
        // Grouped aliases: alias ( ... )
        while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
            int start_pos = pos;
            if (current()->type == TOKEN_COMMA) {
                advance(); 
                continue;
            }
            parse_single_alias(program);

            if (pos == start_pos) {
                 printf("Error: Unexpected token in alias: %s\n", current()->text);
                 advance();
            }
        }
        expect(TOKEN_RPAREN);
    } else {
        // Single alias
        parse_single_alias(program);
    }
}

// Coroutine declaration (COIL-120, spec 9.1):
//   coroutine name(params) { ... }
// Produces an AST_FUNCTION marked is_coroutine with return type
// "void" (coroutines return no value). Parameter parsing mirrors
// the ordinary function path, minus fn-declarator parameters.
static void parse_coroutine_decl(ASTNode* program) {
    advance(); // 'coroutine'
    if (!expect(TOKEN_IDENTIFIER)) {
        printf("Error: coroutine needs a name\n");
        return;
    }
    ASTNode* func = ast_new(AST_FUNCTION);
    func->is_coroutine = 1;
    strcpy(func->text, tokens.tokens[pos - 1].text);

    ASTNode* ret_node = ast_new(AST_IDENTIFIER);
    strcpy(ret_node->text, "void");
    func->children[func->child_count++] = ret_node;

    expect(TOKEN_LPAREN);
    while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
        if (current()->type == TOKEN_COMMA) { advance(); continue; }
        if (current()->type == TOKEN_CONST) advance();

        if (current()->type == TOKEN_AND && pos + 1 < tokens.count &&
            tokens.tokens[pos + 1].type == TOKEN_LPAREN) {
            // COIL-136: a reference would dangle across yields.
            printf("Error: a coroutine cannot take a reference parameter - pass the struct by value, or send it through a link (line %d)\n", current()->line);
            g_parse_errors++;
            advance(); // &
            advance(); // (
            while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) advance();
            if (current()->type == TOKEN_RPAREN) advance();
            if (current()->type == TOKEN_IDENTIFIER) advance(); // the name
            continue;
        }
        char arg_type[256];
        if (current()->type == TOKEN_STRUCT) {
            advance();
            snprintf(arg_type, sizeof(arg_type), "struct %.150s", current()->text);
            advance();
        } else {
            strcpy(arg_type, current()->text);
            advance();
        }
        int psize1 = peek_array_size();
        if (match(TOKEN_LBRACKET)) {
            while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
            expect(TOKEN_RBRACKET);
            strcat(arg_type, "[]");
        }
        if (strcmp(arg_type, "map") == 0 && current()->type == TOKEN_LBRACE) {
            parse_map_braces_text(arg_type, sizeof(arg_type));
        }
        if (current()->type == TOKEN_IDENTIFIER) {
            ASTNode* arg = ast_new(AST_VAR_DECL);
            strcpy(arg->text, current()->text);
            advance();
            int psize2 = peek_array_size();
            if (match(TOKEN_LBRACKET)) {
                while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
                expect(TOKEN_RBRACKET);
                strcat(arg_type, "[]");
            }
            ASTNode* at = ast_new(AST_IDENTIFIER);
            strcpy(at->text, arg_type);
            at->array_size = psize2 ? psize2 : psize1;
            arg->children[arg->child_count++] = NULL; // No init
            arg->children[arg->child_count++] = at;
            func->children[func->child_count++] = arg;
        } else {
            printf("Error: coroutine parameter needs a name\n");
            advance();
        }
    }
    expect(TOKEN_RPAREN);

    if (current()->type == TOKEN_LBRACE) {
        ASTNode* body = parse_block();
        func->children[func->child_count++] = body;
        program->children[program->child_count++] = func;
    } else {
        printf("Error: coroutine '%s' needs a body\n", func->text);
    }
}

static void parse_top_level_decl(ASTNode* program) {
    Token* t = current();

    if (t->type == TOKEN_COROUTINE) {
        parse_coroutine_decl(program);
        return;
    }
    
    char type_name[256] = {0};
    int is_method = 0;
    int implicit_type = 0;

    // A reference type at top level would be a return type or a
    // global variable: neither exists (COIL-136) - references
    // are parameters and local variables only.
    if (t->type == TOKEN_AND && pos + 1 < tokens.count &&
        tokens.tokens[pos + 1].type == TOKEN_LPAREN) {
        printf("Error: a reference type &(struct Name) is only allowed for a function parameter or a local variable - a function cannot return a reference, and a global cannot be one (line %d)\n", t->line);
        g_parse_errors++;
        advance(); // &
        advance(); // (
        while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) advance();
        if (current()->type == TOKEN_RPAREN) advance();
        return;
    }

    // Variable or Function declaration
    // Check if it starts with a type OR is an implicit function definition (e.g. main() or myfunc())
    if (is_type_token(t->type) || t->type == TOKEN_MAIN || t->type == TOKEN_LPAREN ||
        (t->type == TOKEN_IDENTIFIER && tokens.tokens[pos+1].type == TOKEN_LPAREN)) {
             
         // Parse type info
         // int is_struct = 0; // UNUSED

         if (t->type == TOKEN_LPAREN) {
              // Parse tuple type: (int, string) (spec 8.2)
              advance(); // (
              strcpy(type_name, "(");
              while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                  // Keep multi-token types readable: a space
                  // between tokens except after '(' and ','.
                  size_t tl = strlen(type_name);
                  if (tl > 1 && type_name[tl - 1] != ',' && type_name[tl - 1] != '(') {
                      strcat(type_name, " ");
                  }
                  strcat(type_name, current()->text);
                  advance();
                  if (match(TOKEN_COMMA)) strcat(type_name, ",");
                  else break;
              }
              expect(TOKEN_RPAREN);
              strcat(type_name, ")");
         } else if (t->type == TOKEN_STRUCT) {
             // is_struct = 1;
             advance();
             if (current()->type == TOKEN_IDENTIFIER) {
                 sprintf(type_name, "struct %.150s", current()->text);
                 advance();
             } else {
                 // struct { ... } ?
                 strcpy(type_name, "struct");
             }
         } else if (t->type == TOKEN_MAIN || (t->type == TOKEN_IDENTIFIER && tokens.tokens[pos+1].type == TOKEN_LPAREN)) {
             // "main()" or "func()" -> Implicit return type
             strcpy(type_name, (strcmp(t->text, "main")==0) ? "int" : "void");
             implicit_type = 1;
         } else {
             strcpy(type_name, t->text);
             advance();
             // Map type: map{K, V} (COIL-142) - the top-level
             // path missed the braces parse_var_decl has had
             // since COIL-102, so a module-level map variable
             // died on the '{' as an unknown top-level token.
             if (strcmp(type_name, "map") == 0 && current()->type == TOKEN_LBRACE) {
                 parse_map_braces_text(type_name, sizeof(type_name));
             }
             // Check array [] in type? "int[] x" or "int[16] x"
             if (match(TOKEN_LBRACKET)) {
                 while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
                 expect(TOKEN_RBRACKET);
                 strcat(type_name, "[]");
             }
         }
         
         // Handle explicit vs implicit flow
         char name[256];
         int is_func_def = 0;
         
         if (implicit_type) {
             strcpy(name, t->text);
             advance();
             is_func_def = 1; 
         } else {
         if (current()->type == TOKEN_IDENTIFIER || current()->type == TOKEN_MAIN) {
              strcpy(name, current()->text);
              advance();
              
              // Check for "Struct.Method" syntax
              if (current()->type == TOKEN_DOT) {
                  advance(); // consume DOT
                  if (match(TOKEN_IDENTIFIER)) { // Use match/expect for identifier
                      char method_name[64];
                      char struct_name[64];
                      
                      strcpy(method_name, tokens.tokens[pos-1].text);
                         
                         // Determine Struct Name (it's in 'name' currently)
                         strcpy(struct_name, name);
                         
                         // Mangled Name: Struct_Method
                         sprintf(name, "%s_%s", struct_name, method_name);
                         is_method = 1;
                         
                         // We need to inject 'self' argument. 
                         // But we are outside the func parsing block.
                         // We can abuse 'implicit_type' flag or add a new one?
                         // Let's add 'is_method_def' to the loop scope? 
                         // No, variables must be declared at top of block in C90 (if strictly followed, but here we can mix).
                         // We need to pass this info to the arg parsing section.
                         // Since we can't easily add a variable to outer scope without refactoring, 
                         // let's assume if name has underscore and looks like struct method, we might want to check?
                         // Better: Refactor `parse_top_level_decl` variable declarations.
                     }
                 }

                 is_func_def = 1; 
             }
         }
         
         if (is_func_def) {
             if (current()->type == TOKEN_LPAREN) {
                 // Function definition: Type Name(...) { ... }
                 // OR Prototype: Type Name(...);
                 ASTNode* func = ast_new(AST_FUNCTION);
                 strcpy(func->text, name); // Function name
                 
                 // Child 0: Return Type
                 ASTNode* ret_node = ast_new(AST_IDENTIFIER);
                 strcpy(ret_node->text, type_name);
                 func->children[func->child_count++] = ret_node;
                 
                 expect(TOKEN_LPAREN);
                 
                 // Inject 'self' argument if method
                 if (is_method) {
                      // Extract struct name from mangled name or re-derive?
                      // Name is "Struct_Method". We need "Struct".
                      char struct_name[64];
                      char* underscore;
                      ASTNode* self_arg;
                      ASTNode* type_node;

                      strcpy(struct_name, name);
                      underscore = strrchr(struct_name, '_');
                      if (underscore) *underscore = 0;
                      
                      self_arg = ast_new(AST_VAR_DECL);
                      strcpy(self_arg->text, "self");
                      self_arg->children[self_arg->child_count++] = NULL; // No init
                      
                      type_node = ast_new(AST_IDENTIFIER);
                      sprintf(type_node->text, "%s*", struct_name); // Pointer to struct (or typedef)
                      self_arg->children[self_arg->child_count++] = type_node;
                      
                      func->children[func->child_count++] = self_arg;
                      
                      // Check for comma if there are more args
                      if (current()->type != TOKEN_RPAREN) {
                          // We don't need to check comma here because the loop below expects args?
                          // But wait, "byte TCP_ADDR.nport()". No args in parens.
                          // parser loop: while (current != RPAREN).
                          // So it will skip loop.
                          // But if there ARE args: "func(a)".
                          // We injected self. So effectively "func(self, a)".
                          // The loop handles "a".
                          // Does loop require comma at start?
                          // Loop: if (COMMA) advance.
                          // So if user wrote "func(a)", we injected self.
                          // Next token is "a". Loop sees "a". Logic parses "a".
                          // Correct.
                          // But what if user wrote "func()"?
                          // injected self. Next is RPAREN. Loop terminates.
                          // Correct.
                      }
                 }

                 // Parse Args
                 // arg: Type Name
                 while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                      if (current()->type == TOKEN_COMMA) { advance(); continue; }
                      if (current()->type == TOKEN_CONST) advance(); // skip const in args for now
                      
                      char arg_type[256];
                      int is_ref_param = 0;
                      if (current()->type == TOKEN_AND && pos + 1 < tokens.count &&
                          tokens.tokens[pos + 1].type == TOKEN_LPAREN) {
                          // Reference parameter (COIL-136):
                          // &(struct Name) p - non-owning.
                          advance(); // &
                          advance(); // (
                          if (current()->type == TOKEN_STRUCT && pos + 1 < tokens.count &&
                              tokens.tokens[pos + 1].type == TOKEN_IDENTIFIER) {
                              advance(); // struct
                              snprintf(arg_type, sizeof(arg_type), "&(struct %.150s)", current()->text);
                              advance(); // Name
                              is_ref_param = 1;
                          } else {
                              printf("Error: '&(' must wrap a struct type: &(struct Name) (line %d)\n", current()->line);
                              g_parse_errors++;
                              strcpy(arg_type, "int");
                          }
                          if (!expect(TOKEN_RPAREN)) break;
                      } else if (current()->type == TOKEN_STRUCT) {
                          advance();
                          sprintf(arg_type, "struct %.150s", current()->text);
                          advance();
                      } else {
                          strcpy(arg_type, current()->text);
                          advance();
                      }
                      // brackets?
                      int psize1 = is_ref_param ? 0 : peek_array_size();
                      if (!is_ref_param && match(TOKEN_LBRACKET)) {
                          while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
                          expect(TOKEN_RBRACKET);
                          strcat(arg_type, "[]");
                      }
                      // map{K, V} parameter type (COIL-102)
                      if (!is_ref_param && strcmp(arg_type, "map") == 0 && current()->type == TOKEN_LBRACE) {
                          parse_map_braces_text(arg_type, sizeof(arg_type));
                      }
                      
                      if (current()->type == TOKEN_IDENTIFIER) {
                          ASTNode* lb = match(TOKEN_LBRACKET) ? ast_new(AST_NUMBER) : NULL; // check array after name?
                          if (lb) match(TOKEN_RBRACKET);
                          
                          ASTNode* arg = ast_new(AST_VAR_DECL);
                          strcpy(arg->text, current()->text);
                          advance();
                          
                          // Add type to arg
                          ASTNode* at = ast_new(AST_IDENTIFIER);
                          strcpy(at->text, arg_type);
                          
                          // Check array after name
                          int is_arr = 0;
                          int psize2 = peek_array_size();
                          if (match(TOKEN_LBRACKET)) {
                              while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
                              expect(TOKEN_RBRACKET);
                              is_arr = 1;
                          }

                          if (is_arr) strcat(at->text, "[]"); // array param
                          // Function-declarator parameter (COIL-109,
                          // spec 8.3): int compare(int a, int b).
                          // The type is recorded as fn:<ret>(<types>).
                          if (current()->type == TOKEN_LPAREN) {
                              char fn_type[512];
                              snprintf(fn_type, sizeof(fn_type), "fn:%.*s(", (int)sizeof(fn_type) - 5, at->text);
                              advance(); // (
                              int first = 1;
                              while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
                                  if (current()->type == TOKEN_COMMA) { advance(); continue; }
                                  if (current()->type == TOKEN_CONST) advance();
                                  char pt[160];
                                  if (current()->type == TOKEN_STRUCT) {
                                      advance();
                                      snprintf(pt, sizeof(pt), "struct %.150s", current()->text);
                                      advance();
                                  } else {
                                      snprintf(pt, sizeof(pt), "%.150s", current()->text);
                                      advance();
                                  }
                                  if (match(TOKEN_LBRACKET)) {
                                      while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
                                      expect(TOKEN_RBRACKET);
                                      strcat(pt, "[]");
                                  }
                                  if (current()->type == TOKEN_IDENTIFIER) advance(); // inner param name
                                  if (match(TOKEN_LBRACKET)) {
                                      while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
                                      expect(TOKEN_RBRACKET);
                                      strcat(pt, "[]");
                                  }
                                  if (!first) strcat(fn_type, ",");
                                  strcat(fn_type, pt);
                                  first = 0;
                              }
                              expect(TOKEN_RPAREN);
                              strcat(fn_type, ")");
                              strcpy(at->text, fn_type);
                          }
                          at->array_size = psize2 ? psize2 : psize1;
                          arg->children[arg->child_count++] = NULL; // No init
                          arg->children[arg->child_count++] = at;
                          
                          func->children[func->child_count++] = arg;
                      }
                 }
                 expect(TOKEN_RPAREN);
                 
                 if (current()->type == TOKEN_LBRACE) {
                     ASTNode* body = parse_block();
                     func->children[func->child_count++] = body;
                     program->children[program->child_count++] = func;
                 } else {
                     // Prototype (semicolon or newline)
                     // Ignore prototypes for AST? Or emit decl?
                     // Emit decl.
                     // Add dummy body?
                     // Mark as prototype?
                     // For now skip prototypes.
                 }
             } else {
                 // Variable Declaration: Type Name [= ...]
                 
                 if (implicit_type && current()->type != TOKEN_LPAREN) {
                      printf("Error: Implicit type only supported for functions (e.g. 'main()'). Got '%s' after '%s'\n", current()->text, name);
                 }

                 ASTNode* var = ast_new(AST_VAR_DECL);
                 strcpy(var->text, name);
                 ASTNode* init = NULL;
                 if (match(TOKEN_ASSIGN)) {
                     init = parse_expression();
                 } else {
                     init = ast_new(AST_NUMBER);
                     strcpy(init->text, "0");
                 }
                 var->children[var->child_count++] = init;
                 ASTNode* type_node = ast_new(AST_IDENTIFIER);
                 strcpy(type_node->text, type_name);
                 // check array
                 int top_arr_size = peek_array_size();
                 if (match(TOKEN_LBRACKET)) {
                     if (current()->type != TOKEN_RBRACKET) {
                          // int arr[10]
                          while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
                     }
                     expect(TOKEN_RBRACKET);
                     strcat(type_node->text, "[]");
                 }
                 type_node->array_size = top_arr_size;
                 var->children[var->child_count++] = type_node;
                 program->children[program->child_count++] = var;
                 match(TOKEN_SEMICOLON); // optional ;
             }
         }
     } else {
         // Unknown top-level construct. This used to eat one
         // token and continue, which silently dissolved
         // whole declarations (an "interface Shape { ... }"
         // block built without a trace). Name it and fail
         // (COIL-128: the grammar excludes it, so the parser
         // must too).
         if (t->type != TOKEN_SEMICOLON) {
             printf("Error: unexpected top-level token '%s' at line %d: not a declaration the grammar allows\n",
                    t->text, t->line);
             g_parse_errors++;
         }
         advance(); // unknown top level
     }
}

int parse_file(const char* filename, ASTNode** out_ast) {
    if (lex_file(filename, &tokens) != 0) return 1;
    pos = 0;
    g_parse_errors = 0;

    // The grammar starts every file with a module
    // declaration (spec 3); the parser used to accept
    // files without one. Enforce it (COIL-128).
    if (tokens.count == 0 || tokens.tokens[0].type != TOKEN_MODULE) {
        printf("Error: %s: a Come file must begin with a module declaration ('module <name>')\n",
               filename);
        g_parse_errors++;
    }
    
    *out_ast = ast_new(AST_PROGRAM);
    
    while (pos < tokens.count) {
        Token* t = current();
        if (t->type == TOKEN_EOF) break;
        
        switch (t->type) {
            case TOKEN_MODULE:
                advance(); // module
                if (current()->type == TOKEN_DOT) {
                    advance(); // .
                    if (strcmp(current()->text, "init") == 0) {
                        advance(); // init
                        expect(TOKEN_LPAREN);
                        expect(TOKEN_RPAREN);
                        
                        ASTNode* init_func = ast_new(AST_FUNCTION);
                        strcpy(init_func->text, "module_init");
                        
                        // Return type: void
                        ASTNode* ret = ast_new(AST_IDENTIFIER);
                        strcpy(ret->text, "void");
                        init_func->children[init_func->child_count++] = ret;
                        
                        // No args for now in module.init()
                        
                        if (current()->type == TOKEN_LBRACE) {
                            ASTNode* body = parse_block();
                            init_func->children[init_func->child_count++] = body;
                            (*out_ast)->children[(*out_ast)->child_count++] = init_func;
                        }
                    }
                } else if (current()->type == TOKEN_MAIN || current()->type == TOKEN_IDENTIFIER || 
                           current()->type == TOKEN_STRING || current()->type == TOKEN_MAP) {
                     strncpy((*out_ast)->text, current()->text, 255);
                     advance();
                }
                break;

            case TOKEN_IMPORT:
                parse_import(*out_ast);
                break;
            case TOKEN_EXTERN:
                parse_extern(*out_ast);
                break;
            case TOKEN_EXPORT:
                parse_export(*out_ast);
                break;
            case TOKEN_CONST:
                parse_const(*out_ast);
                break;
            case TOKEN_UNION:
                parse_union(*out_ast);
                break;
            case TOKEN_STRUCT:
                if (pos + 2 < tokens.count && tokens.tokens[pos+1].type == TOKEN_IDENTIFIER && tokens.tokens[pos+2].type == TOKEN_LBRACE) {
                    parse_struct(*out_ast);
                } else {
                    parse_top_level_decl(*out_ast);
                }
                break;
            case TOKEN_ALIAS:
                parse_alias(*out_ast);
                break;
            default:
                // Struct definition with layout modifiers (COIL-104)
                if (at_struct_modifier()) {
                    int m_align = 0;
                    int m_packed = parse_struct_modifiers(&m_align);
                    if (current()->type == TOKEN_STRUCT && pos + 2 < tokens.count &&
                        tokens.tokens[pos + 1].type == TOKEN_IDENTIFIER &&
                        tokens.tokens[pos + 2].type == TOKEN_LBRACE) {
                        int before = (*out_ast)->child_count;
                        parse_struct(*out_ast);
                        if ((*out_ast)->child_count > before) {
                            ASTNode* sn = (*out_ast)->children[(*out_ast)->child_count - 1];
                            sn->layout_packed = m_packed;
                            sn->layout_align = m_align;
                        }
                        break;
                    }
                }
                parse_top_level_decl(*out_ast);
                break;
        }
    }

    return g_parse_errors > 0 ? 2 : 0;
}
