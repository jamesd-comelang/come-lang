#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

int lex_file(const char* filename, TokenList* out) {
    FILE* f = fopen(filename, "r");
    if (!f) { perror("Cannot open file"); return 1; }
    out->count = 0;
    char line[256];
    int in_block_comment = 0;
    int line_num = 1;  // Track current line number for source mapping
    while(fgets(line,sizeof(line),f)) {
        char* p = line;
        while(*p) {
            if (in_block_comment) {
                while (*p) {
                     if (*p == '*' && *(p+1) == '/') {
                         in_block_comment = 0;
                         p += 2; 
                         break;
                     }
                     p++;
                }
                continue;
            }

            if(isspace(*p)){ p++; continue; }
            // Skip comments
            if(*p=='/' && *(p+1)=='/') { while(*p && *p!='\n') p++; continue; }
            if(*p=='/' && *(p+1)=='*') { 
                in_block_comment = 1; 
                p+=2; 
                if (!*p) continue; // End of line inside comment
                // Check if it closes on same line
                while (*p) {
                     if (*p == '*' && *(p+1) == '/') {
                         in_block_comment = 0;
                         p += 2; 
                         break;
                     }
                     p++;
                }
                if (in_block_comment) continue; 
                // If closed, continue loop to parse next token
            }
            
            Token tok;
            tok.text[0]='\0'; tok.type=TOKEN_UNKNOWN; tok.line=line_num;
            
            // Helper for keyword matching with boundary check
            #define MATCH_KEYWORD(str, type_val) \
                (strncmp(p, str, strlen(str))==0 && !isalnum(p[strlen(str)]) && p[strlen(str)]!='_')

            if(MATCH_KEYWORD("import", TOKEN_IMPORT)) { tok.type=TOKEN_IMPORT; strcpy(tok.text,"import"); p+=6; }
            else if(MATCH_KEYWORD("module", TOKEN_MODULE)) { tok.type=TOKEN_MODULE; strcpy(tok.text,"module"); p+=6; }
            else if(MATCH_KEYWORD("main", TOKEN_MAIN)) { tok.type=TOKEN_MAIN; strcpy(tok.text,"main"); p+=4; }
            else if(MATCH_KEYWORD("const", TOKEN_CONST)) { tok.type=TOKEN_CONST; strcpy(tok.text,"const"); p+=5; }
            else if(MATCH_KEYWORD("enum", TOKEN_ENUM)) { tok.type=TOKEN_ENUM; strcpy(tok.text,"enum"); p+=4; }
            else if(MATCH_KEYWORD("union", TOKEN_UNION)) { tok.type=TOKEN_UNION; strcpy(tok.text,"union"); p+=5; }
            else if(MATCH_KEYWORD("struct", TOKEN_STRUCT)) { tok.type=TOKEN_STRUCT; strcpy(tok.text,"struct"); p+=6; }
            else if(MATCH_KEYWORD("alias", TOKEN_ALIAS)) { tok.type=TOKEN_ALIAS; strcpy(tok.text,"alias"); p+=5; }
            else if(MATCH_KEYWORD("method", TOKEN_METHOD)) { tok.type=TOKEN_METHOD; strcpy(tok.text,"method"); p+=6; }
            else if(MATCH_KEYWORD("export", TOKEN_EXPORT)) { tok.type=TOKEN_EXPORT; strcpy(tok.text,"export"); p+=6; }
            else if(MATCH_KEYWORD("var", TOKEN_VAR)) { tok.type=TOKEN_VAR; strcpy(tok.text,"var"); p+=3; }
            
            else if(MATCH_KEYWORD("switch", TOKEN_SWITCH)) { tok.type=TOKEN_SWITCH; strcpy(tok.text,"switch"); p+=6; }
            else if(MATCH_KEYWORD("case", TOKEN_CASE)) { tok.type=TOKEN_CASE; strcpy(tok.text,"case"); p+=4; }
            else if(MATCH_KEYWORD("default", TOKEN_DEFAULT)) { tok.type=TOKEN_DEFAULT; strcpy(tok.text,"default"); p+=7; }
            else if(MATCH_KEYWORD("fallthrough", TOKEN_FALLTHROUGH)) { tok.type=TOKEN_FALLTHROUGH; strcpy(tok.text,"fallthrough"); p+=11; }
            
            else if(MATCH_KEYWORD("for", TOKEN_FOR)) { tok.type=TOKEN_FOR; strcpy(tok.text,"for"); p+=3; }
            else if(MATCH_KEYWORD("while", TOKEN_WHILE)) { tok.type=TOKEN_WHILE; strcpy(tok.text,"while"); p+=5; }
            else if(MATCH_KEYWORD("do", TOKEN_DO)) { tok.type=TOKEN_DO; strcpy(tok.text,"do"); p+=2; }
            else if(MATCH_KEYWORD("return", TOKEN_RETURN)) { tok.type=TOKEN_RETURN; strcpy(tok.text,"return"); p+=6; }
            else if(MATCH_KEYWORD("if", TOKEN_IF)) { tok.type=TOKEN_IF; strcpy(tok.text,"if"); p+=2; }
            else if(MATCH_KEYWORD("else", TOKEN_ELSE)) { tok.type=TOKEN_ELSE; strcpy(tok.text,"else"); p+=4; }
            else if(MATCH_KEYWORD("break", TOKEN_BREAK)) { tok.type=TOKEN_BREAK; strcpy(tok.text,"break"); p+=5; }
            else if(MATCH_KEYWORD("continue", TOKEN_CONTINUE)) { tok.type=TOKEN_CONTINUE; strcpy(tok.text,"continue"); p+=8; }
            
            // Types
            else if(MATCH_KEYWORD("int", TOKEN_INT)) { tok.type=TOKEN_INT; strcpy(tok.text,"int"); p+=3; }
            else if(MATCH_KEYWORD("uint", TOKEN_UINT)) { tok.type=TOKEN_UINT; strcpy(tok.text,"uint"); p+=4; }
            else if(MATCH_KEYWORD("i32", TOKEN_INT)) { tok.type=TOKEN_INT; strcpy(tok.text,"int"); p+=3; }
            else if(MATCH_KEYWORD("u32", TOKEN_UINT)) { tok.type=TOKEN_UINT; strcpy(tok.text,"uint"); p+=3; }
            else if(MATCH_KEYWORD("byte", TOKEN_BYTE)) { tok.type=TOKEN_BYTE; strcpy(tok.text,"byte"); p+=4; }
            else if(MATCH_KEYWORD("i8", TOKEN_BYTE)) { tok.type=TOKEN_BYTE; strcpy(tok.text,"byte"); p+=2; }
            else if(MATCH_KEYWORD("ubyte", TOKEN_UBYTE)) { tok.type=TOKEN_UBYTE; strcpy(tok.text,"ubyte"); p+=5; }
            else if(MATCH_KEYWORD("u8", TOKEN_UBYTE)) { tok.type=TOKEN_UBYTE; strcpy(tok.text,"ubyte"); p+=2; }
            else if(MATCH_KEYWORD("short", TOKEN_SHORT)) { tok.type=TOKEN_SHORT; strcpy(tok.text,"short"); p+=5; }
            else if(MATCH_KEYWORD("i16", TOKEN_SHORT)) { tok.type=TOKEN_SHORT; strcpy(tok.text,"short"); p+=3; }
            else if(MATCH_KEYWORD("ushort", TOKEN_USHORT)) { tok.type=TOKEN_USHORT; strcpy(tok.text,"ushort"); p+=6; }
            else if(MATCH_KEYWORD("u16", TOKEN_USHORT)) { tok.type=TOKEN_USHORT; strcpy(tok.text,"ushort"); p+=3; }
            else if(MATCH_KEYWORD("long", TOKEN_LONG)) { tok.type=TOKEN_LONG; strcpy(tok.text,"long"); p+=4; }
            else if(MATCH_KEYWORD("i64", TOKEN_LONG)) { tok.type=TOKEN_LONG; strcpy(tok.text,"long"); p+=3; }
            else if(MATCH_KEYWORD("ulong", TOKEN_ULONG)) { tok.type=TOKEN_ULONG; strcpy(tok.text,"ulong"); p+=5; }
            else if(MATCH_KEYWORD("u64", TOKEN_ULONG)) { tok.type=TOKEN_ULONG; strcpy(tok.text,"ulong"); p+=3; }
            else if(MATCH_KEYWORD("float", TOKEN_FLOAT)) { tok.type=TOKEN_FLOAT; strcpy(tok.text,"float"); p+=5; }
            else if(MATCH_KEYWORD("f32", TOKEN_FLOAT)) { tok.type=TOKEN_FLOAT; strcpy(tok.text,"float"); p+=3; }
            else if(MATCH_KEYWORD("double", TOKEN_DOUBLE)) { tok.type=TOKEN_DOUBLE; strcpy(tok.text,"double"); p+=6; }
            else if(MATCH_KEYWORD("f64", TOKEN_DOUBLE)) { tok.type=TOKEN_DOUBLE; strcpy(tok.text,"double"); p+=3; }
            else if(MATCH_KEYWORD("void", TOKEN_VOID)) { tok.type=TOKEN_VOID; strcpy(tok.text,"void"); p+=4; }
            else if(MATCH_KEYWORD("wchar", TOKEN_WCHAR)) { tok.type=TOKEN_WCHAR; strcpy(tok.text,"wchar"); p+=5; }
            else if(MATCH_KEYWORD("bool", TOKEN_BOOL)) { tok.type=TOKEN_BOOL; strcpy(tok.text,"bool"); p+=4; }
            else if(MATCH_KEYWORD("string", TOKEN_STRING)) { tok.type=TOKEN_STRING; strcpy(tok.text,"string"); p+=6; }
            else if(MATCH_KEYWORD("map", TOKEN_MAP)) { tok.type=TOKEN_MAP; strcpy(tok.text,"map"); p+=3; }
            
            else if(MATCH_KEYWORD("true", TOKEN_TRUE)) { tok.type=TOKEN_TRUE; strcpy(tok.text,"true"); p+=4; }
            else if(MATCH_KEYWORD("false", TOKEN_FALSE)) { tok.type=TOKEN_FALSE; strcpy(tok.text,"false"); p+=5; }
            
            
            // Symbols
            else if(*p=='('){ tok.type=TOKEN_LPAREN; strcpy(tok.text,"("); p++; }
            else if(*p==')'){ tok.type=TOKEN_RPAREN; strcpy(tok.text,")"); p++; }
            else if(*p=='{'){ tok.type=TOKEN_LBRACE; strcpy(tok.text,"{"); p++; }
            else if(*p=='}'){ tok.type=TOKEN_RBRACE; strcpy(tok.text,"}"); p++; }
            else if(*p=='['){ tok.type=TOKEN_LBRACKET; strcpy(tok.text,"["); p++; }
            else if(*p==']'){ tok.type=TOKEN_RBRACKET; strcpy(tok.text,"]"); p++; }
            else if(*p=='.'){ tok.type=TOKEN_DOT; strcpy(tok.text,"."); p++; }
            else if(*p==':'){ tok.type=TOKEN_COLON; strcpy(tok.text,":"); p++; }
            else if(*p==';'){ tok.type=TOKEN_SEMICOLON; strcpy(tok.text,";"); p++; }
            else if(*p==','){ tok.type=TOKEN_COMMA; strcpy(tok.text,","); p++; }
            else if(*p=='?'){ tok.type=TOKEN_QUESTION; strcpy(tok.text,"?"); p++; }
            else if(*p=='~'){ tok.type=TOKEN_TILDE; strcpy(tok.text,"~"); p++; }
            
            // Multi-char Operators
            else if(strncmp(p, "<<=", 3)==0) { tok.type=TOKEN_LSHIFT_ASSIGN; strcpy(tok.text,"<<="); p+=3; }
            else if(strncmp(p, ">>=", 3)==0) { tok.type=TOKEN_RSHIFT_ASSIGN; strcpy(tok.text,">>="); p+=3; }
            else if(strncmp(p, "<<", 2)==0) { tok.type=TOKEN_LSHIFT; strcpy(tok.text,"<<"); p+=2; }
            else if(strncmp(p, ">>", 2)==0) { tok.type=TOKEN_RSHIFT; strcpy(tok.text,">>"); p+=2; }
            else if(strncmp(p, "&&", 2)==0) { tok.type=TOKEN_LOGIC_AND; strcpy(tok.text,"&&"); p+=2; }
            else if(strncmp(p, "||", 2)==0) { tok.type=TOKEN_LOGIC_OR; strcpy(tok.text,"||"); p+=2; }
            else if(strncmp(p, "==", 2)==0) { tok.type=TOKEN_EQ; strcpy(tok.text,"=="); p+=2; }
            else if(strncmp(p, "!=", 2)==0) { tok.type=TOKEN_NEQ; strcpy(tok.text,"!="); p+=2; }
            else if(strncmp(p, ">=", 2)==0) { tok.type=TOKEN_GE; strcpy(tok.text,">="); p+=2; }
            else if(strncmp(p, "<=", 2)==0) { tok.type=TOKEN_LE; strcpy(tok.text,"<="); p+=2; }
            else if(strncmp(p, "+=", 2)==0) { tok.type=TOKEN_PLUS_ASSIGN; strcpy(tok.text,"+="); p+=2; }
            else if(strncmp(p, "-=", 2)==0) { tok.type=TOKEN_MINUS_ASSIGN; strcpy(tok.text,"-="); p+=2; }
            else if(strncmp(p, "*=", 2)==0) { tok.type=TOKEN_STAR_ASSIGN; strcpy(tok.text,"*="); p+=2; }
            else if(strncmp(p, "/=", 2)==0) { tok.type=TOKEN_SLASH_ASSIGN; strcpy(tok.text,"/="); p+=2; }
            else if(strncmp(p, "%=", 2)==0) { tok.type=TOKEN_MOD_ASSIGN; strcpy(tok.text,"%="); p+=2; }
            else if(strncmp(p, "&=", 2)==0) { tok.type=TOKEN_AND_ASSIGN; strcpy(tok.text,"&="); p+=2; }
            else if(strncmp(p, "|=", 2)==0) { tok.type=TOKEN_OR_ASSIGN; strcpy(tok.text,"|="); p+=2; }
            else if(strncmp(p, "^=", 2)==0) { tok.type=TOKEN_XOR_ASSIGN; strcpy(tok.text,"^="); p+=2; }
            else if(strncmp(p, "++", 2)==0) { tok.type=TOKEN_INC; strcpy(tok.text,"++"); p+=2; }
            else if(strncmp(p, "--", 2)==0) { tok.type=TOKEN_DEC; strcpy(tok.text,"--"); p+=2; }
            
            // Single char ops
            else if(*p=='+'){ tok.type=TOKEN_PLUS; strcpy(tok.text,"+"); p++; }
            else if(*p=='-'){ tok.type=TOKEN_MINUS; strcpy(tok.text,"-"); p++; }
            else if(*p=='*'){ tok.type=TOKEN_STAR; strcpy(tok.text,"*"); p++; }
            else if(*p=='/'){ tok.type=TOKEN_SLASH; strcpy(tok.text,"/"); p++; }
            else if(*p=='%'){ tok.type=TOKEN_PERCENT; strcpy(tok.text,"%"); p++; }
            else if(*p=='&'){ tok.type=TOKEN_AND; strcpy(tok.text,"&"); p++; }
            else if(*p=='|'){ tok.type=TOKEN_OR; strcpy(tok.text,"|"); p++; }
            else if(*p=='^'){ tok.type=TOKEN_XOR; strcpy(tok.text,"^"); p++; }
            else if(*p=='!'){ tok.type=TOKEN_NOT; strcpy(tok.text,"!"); p++; }
            else if(*p=='>'){ tok.type=TOKEN_GT; strcpy(tok.text,">"); p++; }
            else if(*p=='<'){ tok.type=TOKEN_LT; strcpy(tok.text,"<"); p++; }
            else if(*p=='='){ tok.type=TOKEN_ASSIGN; strcpy(tok.text,"="); p++; }
            
            // Literals
            else if(isdigit(*p)) { 
                int i=0; 
                if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) {
                    tok.text[i++] = *p++; // 0
                    tok.text[i++] = *p++; // x
                    while(isxdigit(*p)) tok.text[i++] = *p++;
                } else {
                    while(isdigit(*p) || *p == '\'') {
                        if (*p != '\'') tok.text[i++] = *p; 
                        p++;
                    }
                    if(*p == '.' && isdigit(*(p+1))) {
                        tok.text[i++] = *p++;
                        while(isdigit(*p) || *p == '\'') {
                             if (*p != '\'') tok.text[i++] = *p;
                             p++;
                        }
                    }
                }
                // Handle suffixes: L, LL, f, u, etc.
                while (*p == 'L' || *p == 'f' || *p == 'u' || *p == 'U') {
                    tok.text[i++] = *p++;
                }
                tok.text[i] = '\0'; 
                tok.type = TOKEN_NUMBER; 
            }
            else if(*p=='"'){ int i=0; tok.text[i++]=*p++; while(*p && *p!='"') tok.text[i++]=*p++; if(*p=='"') tok.text[i++]=*p++; tok.text[i]='\0'; tok.type=TOKEN_STRING_LITERAL; }
            else if(*p=='\''){ int i=0; tok.text[i++]=*p++; while(*p && *p!='\'') tok.text[i++]=*p++; if(*p=='\'') tok.text[i++]=*p++; tok.text[i]='\0'; tok.type=TOKEN_CHAR_LITERAL; } // Changed to CHAR_LITERAL default, distinction handled in parsing?
            
            else if(isalpha(*p) || *p=='_'){ int i=0; while(isalnum(*p)||*p=='_') tok.text[i++]=*p++; tok.text[i]='\0'; tok.type=TOKEN_IDENTIFIER; }
            else { p++; continue; }
            out->tokens[out->count++]=tok;
        }
        line_num++;  // Increment line number after processing each line
    }
    fclose(f);
    Token eof={TOKEN_EOF,"", line_num};
    out->tokens[out->count++]=eof;
    return 0;
}
