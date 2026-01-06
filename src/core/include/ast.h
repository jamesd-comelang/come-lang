#ifndef AST_H
#define AST_H
typedef enum {
    AST_PROGRAM,
    AST_FUNCTION,
    AST_BLOCK,
    AST_PRINTF,
    AST_VAR_DECL,
    AST_ASSIGN,
    AST_METHOD_CALL,
    AST_CALL,
    AST_IF,
    AST_ELSE,
    AST_WHILE,
    AST_RETURN,
    AST_STRING_LITERAL,
    AST_BOOL_LITERAL,
    AST_NUMBER,
    AST_IDENTIFIER,
    AST_ARRAY_ACCESS,
    AST_MEMBER_ACCESS,
    AST_TYPE_ALIAS,
    AST_STRUCT_DECL,
    AST_AGGREGATE_INIT, // { k: v, ... } or [ v, ... ]
    
    AST_NET_TCP_CONNECT,    // net.tcp.connect(addr)
    AST_NET_TCP_LISTEN,     // net.tcp.listen(addr)
    AST_NET_TCP_ACCEPT,     // net.tcp.accept(listener)
    AST_NET_TCP_ON,         // conn.on(EVENT) { ... }
    AST_NET_TCP_ADDR,       // net.tcp.Addr(...)
    AST_CONST_DECL,
    AST_CONST_GROUP,
    AST_ENUM_DECL,
    AST_UNION_DECL,
    AST_SWITCH,
    AST_CASE,
    AST_DEFAULT,
    AST_FOR,
    AST_DO_WHILE,
    AST_BINARY_OP,
    AST_UNARY_OP,
    AST_POST_INC,
    AST_POST_DEC,
    AST_EXPORT,
    AST_IMPORT,
    AST_BREAK,
    AST_CONTINUE,
    AST_CAST,
    AST_TERNARY,
    AST_TYPE_END
} ASTNodeType;

typedef struct ASTNode {
    ASTNodeType type;
    char text[4096];
    struct ASTNode* children[1024];
    int child_count;
    int source_line;  // Line number in original COME source file
} ASTNode;
ASTNode* ast_new(ASTNodeType type);
void ast_free(ASTNode* node);
#endif
