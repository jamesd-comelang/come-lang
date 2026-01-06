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

static int expect(TokenType type) {
    if (match(type)) return 1;
    printf("Expected token type %d, got %d ('%s')\n", type, current()->type, current()->text);
    return 0;
}

ASTNode* ast_new(ASTNodeType type) {
    ASTNode* n = malloc(sizeof(ASTNode));
    n->type = type;
    n->child_count = 0;
    n->text[0] = '\0';
    n->source_line = (pos < tokens.count) ? tokens.tokens[pos].line : 0;
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
static ASTNode* parse_primary() {
    ASTNode* node = NULL;
    Token* t = current();
    
    // Handle Unary Ops that are often parsed at primary level in simple parsers, 
    // though conceptually in expression_prec.
    // In this parser, `parse_expression_prec` calls `parse_primary`.
    // Unary ops like !, ~, * are handled here?
    // Snippet 159 showed NOT, TILDE, STAR handling at TOP of parse_primary!
    // I must preserve that.
    if (t->type == TOKEN_NOT || t->type == TOKEN_TILDE || t->type == TOKEN_STAR || t->type == TOKEN_MINUS) {
        TokenType op_type = t->type; 
        advance();
        ASTNode* operand = parse_primary(); // Recursive for **x or - -x
        ASTNode* unary = ast_new(AST_UNARY_OP);
        if (op_type == TOKEN_NOT) strcpy(unary->text, "!");
        else if (op_type == TOKEN_TILDE) strcpy(unary->text, "~");
        else if (op_type == TOKEN_STAR) strcpy(unary->text, "*");
        else if (op_type == TOKEN_MINUS) strcpy(unary->text, "-");
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
    if (t->type == TOKEN_IDENTIFIER) {
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
        // Array initializer: [1, 2, 3]
        node = ast_new(AST_AGGREGATE_INIT);
        strcpy(node->text, "ARRAY");
        while (current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) {
            node->children[node->child_count++] = parse_expression();
            if (!match(TOKEN_COMMA)) break;
        }
        expect(TOKEN_RBRACKET);
    } else if (match(TOKEN_LBRACE)) {
        // Map/Struct initializer: { k: v, ... } or { .field = val, ... }
        node = ast_new(AST_AGGREGATE_INIT);
        strcpy(node->text, "MAP");
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
                 node->children[node->child_count++] = parse_expression();
             }
             if (!match(TOKEN_COMMA)) break;
        }
        expect(TOKEN_RBRACE);
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
                    
                    // Trailing closure
                    if (current()->type == TOKEN_LBRACE) {
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
        } else if (match(TOKEN_LPAREN)) {
            // Function Call: expr(...)   (e.g. func(), arr[0]())
            
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
        } else if (match(TOKEN_INC)) {
            ASTNode* inc = ast_new(AST_POST_INC);
            inc->children[inc->child_count++] = node;
            node = inc;
        } else if (match(TOKEN_DEC)) {
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
    switch (type) {
        case TOKEN_QUESTION: return 1;
        case TOKEN_LOGIC_OR: return 2;
        case TOKEN_LOGIC_AND: return 3;
        case TOKEN_EQ: case TOKEN_NEQ: return 4;
        case TOKEN_LT: case TOKEN_GT: case TOKEN_LE: case TOKEN_GE: return 5;
        case TOKEN_PLUS: case TOKEN_MINUS: return 6;
        case TOKEN_STAR: case TOKEN_SLASH: case TOKEN_PERCENT: return 7;
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

static ASTNode* parse_var_decl() {
    Token* t = current();
    char type_name[128];
    strcpy(type_name, t->text);
    advance();
    
    // Special handling for struct/union: "struct Type varname" or "union Type varname"
    if ((strcmp(type_name, "struct") == 0 || strcmp(type_name, "union") == 0) && current()->type == TOKEN_IDENTIFIER) {
        // Consume the type name
        strcat(type_name, " ");
        strcat(type_name, current()->text);
        advance();
    }
    
    // Check for array type: int[] x
    while (match(TOKEN_LBRACKET)) {
         while(current()->type != TOKEN_RBRACKET && current()->type != TOKEN_EOF) advance();
         expect(TOKEN_RBRACKET);
         strcat(type_name, "[]");
    }

    if (match(TOKEN_IDENTIFIER)) {
        char var_name[64];
        strcpy(var_name, tokens.tokens[pos-1].text);
        
        int is_array = 0;
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
    
    if (!match(TOKEN_RPAREN)) {
            printf("Expected RPAREN after IF condition, got %d ('%s')\n", current()->type, current()->text);
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
         decl->children[decl->child_count++] = type_node;
         if (current()->type == TOKEN_SEMICOLON) advance();
         return decl;
    } 
    
    // Fallback to expression or complex assignment (e.g. arr[i] = val)
    return parse_expression_statement();
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
                 ASTNode* field = parse_statement();
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
    
    // Check for struct definition explicitly
    if (t->type == TOKEN_STRUCT && pos+2 < tokens.count && 
        tokens.tokens[pos+1].type == TOKEN_IDENTIFIER && 
        tokens.tokens[pos+2].type == TOKEN_LBRACE) {
          return parse_struct_statement();
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
        case TOKEN_FALLTHROUGH: advance(); return NULL;
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
    if (match(TOKEN_LPAREN)) {
        while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
            int start_pos = pos;
            if (current()->type == TOKEN_COMMA) {
                advance();
                continue;
            }
            parse_top_level_decl(program);
            
            if (pos == start_pos) {
                 printf("Error: Unexpected token in export: %s\n", current()->text);
                 advance();
            }
        }
        expect(TOKEN_RPAREN);
    } else {
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
             ASTNode* field = parse_statement(); // Reusing var parsing
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
                     ASTNode* field = parse_statement();
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
            // Macro alias: alias SQUARE(x) = ...
             while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) advance(); // skip args
             expect(TOKEN_RPAREN);
             // Assume macro, skip for now
             if (match(TOKEN_ASSIGN)) parse_expression();
        }
        else if (match(TOKEN_ASSIGN)) {
             // alias X = Y
             // Check if Y is type
             if (is_type_token(current()->type) || current()->type == TOKEN_STRUCT || current()->type == TOKEN_UNION) {
                  // Type Alias
                  ASTNode* node = ast_new(AST_TYPE_ALIAS);
                  strcpy(node->text, name);
                  
                  ASTNode* typeNode = ast_new(AST_IDENTIFIER);
                  
                  if (current()->type == TOKEN_STRUCT) {
                      advance();
                      char t[256];
                      snprintf(t, sizeof(t), "struct %s", current()->text);
                      strcpy(typeNode->text, t);
                      advance();
                  } else if (current()->type == TOKEN_UNION) {
                      advance();
                      char t[256];
                      snprintf(t, sizeof(t), "union %s", current()->text);
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

static void parse_alias(ASTNode* program) {
    advance(); // alias
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

static void parse_top_level_decl(ASTNode* program) {
    Token* t = current();
    
    char type_name[256] = {0};
    int is_method = 0;
    int implicit_type = 0;

    // Variable or Function declaration
    // Check if it starts with a type OR is an implicit function definition (e.g. main() or myfunc())
    if (is_type_token(t->type) || t->type == TOKEN_MAIN || 
        (t->type == TOKEN_IDENTIFIER && tokens.tokens[pos+1].type == TOKEN_LPAREN)) {
             
         // Parse type info
         // int is_struct = 0; // UNUSED

         if (t->type == TOKEN_LPAREN) {
              // Parse tuple type: (int, string)
              advance(); // (
              strcpy(type_name, "(");
              while (current()->type != TOKEN_RPAREN && current()->type != TOKEN_EOF) {
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
                 sprintf(type_name, "struct %s", current()->text);
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
                      if (current()->type == TOKEN_STRUCT) {
                          advance();
                          sprintf(arg_type, "struct %s", current()->text);
                          advance();
                      } else {
                          strcpy(arg_type, current()->text);
                          advance();
                      }
                      // brackets?
                      if (match(TOKEN_LBRACKET)) { 
                          while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
                          expect(TOKEN_RBRACKET); 
                          strcat(arg_type, "[]"); 
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
                          if (match(TOKEN_LBRACKET)) {
                              while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
                              expect(TOKEN_RBRACKET); 
                              is_arr = 1;
                          }
                          
                          if (is_arr) strcat(at->text, "[]"); // array param
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
                 if (match(TOKEN_LBRACKET)) {
                     if (current()->type != TOKEN_RBRACKET) {
                          // int arr[10]
                          // size?
                          while(current()->type!=TOKEN_RBRACKET && current()->type!=TOKEN_EOF) advance();
                     }
                     expect(TOKEN_RBRACKET);
                     strcat(type_node->text, "[]");
                 }
                 var->children[var->child_count++] = type_node;
                 program->children[program->child_count++] = var;
                 match(TOKEN_SEMICOLON); // optional ;
             }
         }
     } else {
         advance(); // unknown top level
     }
}

int parse_file(const char* filename, ASTNode** out_ast) {
    if (lex_file(filename, &tokens) != 0) return 1;
    pos = 0;
    
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
                parse_top_level_decl(*out_ast);
                break;
        }
    }
    
    return 0;
}
