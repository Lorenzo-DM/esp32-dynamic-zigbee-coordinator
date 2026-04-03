/*
 * MIT License — jsmn (Serge Zaitsev)
 * Minimal JSON tokenizer, header-only, zero dependencies.
 * https://github.com/zserge/jsmn
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT    = 1,
    JSMN_ARRAY     = 2,
    JSMN_STRING    = 3,
    JSMN_PRIMITIVE = 4,
} jsmntype_t;

typedef enum {
    JSMN_ERROR_NOMEM = -1,
    JSMN_ERROR_INVAL = -2,
    JSMN_ERROR_PART  = -3,
} jsmnerr_t;

typedef struct {
    jsmntype_t type;
    int start;
    int end;
    int size;
#ifdef JSMN_PARENT_LINKS
    int parent;
#endif
} jsmntok_t;

typedef struct {
    unsigned int pos;
    unsigned int toknext;
    int toksuper;
} jsmn_parser;

static void jsmn_init(jsmn_parser *parser) {
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens, const size_t num_tokens) {
    jsmntok_t *tok;
    if (parser->toknext >= num_tokens) return NULL;
    tok = &tokens[parser->toknext++];
    tok->start = tok->end = -1;
    tok->size = 0;
#ifdef JSMN_PARENT_LINKS
    tok->parent = -1;
#endif
    return tok;
}

static void jsmn_fill_token(jsmntok_t *token, const jsmntype_t type, const int start, const int end) {
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js, const size_t len, jsmntok_t *tokens, const size_t num_tokens) {
    jsmntok_t *token;
    int start = parser->pos;
    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        switch (js[parser->pos]) {
        case ':': case '\t': case '\r': case '\n': case ' ':
        case ',': case ']': case '}': goto found;
        default: break;
        }
        if (js[parser->pos] < 32 || js[parser->pos] >= 127) return JSMN_ERROR_INVAL;
    }
found:
    if (tokens == NULL) { parser->pos--; return 0; }
    token = jsmn_alloc_token(parser, tokens, num_tokens);
    if (token == NULL) { parser->pos = start; return JSMN_ERROR_NOMEM; }
    jsmn_fill_token(token, JSMN_PRIMITIVE, start, parser->pos);
#ifdef JSMN_PARENT_LINKS
    token->parent = parser->toksuper;
#endif
    parser->pos--;
    return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js, const size_t len, jsmntok_t *tokens, const size_t num_tokens) {
    jsmntok_t *token;
    int start = parser->pos;
    parser->pos++;
    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        char c = js[parser->pos];
        if (c == '\"') {
            if (tokens == NULL) return 0;
            token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (token == NULL) { parser->pos = start; return JSMN_ERROR_NOMEM; }
            jsmn_fill_token(token, JSMN_STRING, start + 1, parser->pos);
#ifdef JSMN_PARENT_LINKS
            token->parent = parser->toksuper;
#endif
            return 0;
        }
        if (c == '\\' && parser->pos + 1 < len) {
            parser->pos++;
            switch (js[parser->pos]) {
            case '\"': case '/': case '\\': case 'b': case 'f':
            case 'r': case 'n': case 't': break;
            case 'u':
                parser->pos++;
                for (int i = 0; i < 4 && parser->pos < len && js[parser->pos] != '\0'; i++) {
                    if (!((js[parser->pos] >= '0' && js[parser->pos] <= '9') ||
                          (js[parser->pos] >= 'A' && js[parser->pos] <= 'F') ||
                          (js[parser->pos] >= 'a' && js[parser->pos] <= 'f')))
                        return JSMN_ERROR_INVAL;
                    parser->pos++;
                }
                parser->pos--;
                break;
            default: return JSMN_ERROR_INVAL;
            }
        }
    }
    return JSMN_ERROR_PART;
}

static int jsmn_parse(jsmn_parser *parser, const char *js, const size_t len, jsmntok_t *tokens, const unsigned int num_tokens) {
    int r, count = parser->toknext;
    jsmntok_t *token;
    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        char c = js[parser->pos];
        jsmntype_t type;
        switch (c) {
        case '{': case '[':
            count++;
            if (tokens == NULL) break;
            token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (token == NULL) return JSMN_ERROR_NOMEM;
            if (parser->toksuper != -1) tokens[parser->toksuper].size++;
#ifdef JSMN_PARENT_LINKS
            token->parent = parser->toksuper;
#endif
            token->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
            token->start = parser->pos;
            parser->toksuper = parser->toknext - 1;
            break;
        case '}': case ']':
            type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
            if (tokens == NULL) break;
            if (parser->toknext < 1) return JSMN_ERROR_INVAL;
            token = &tokens[parser->toknext - 1];
            for (;;) {
                if (token->start != -1 && token->end == -1) {
                    if (token->type != type) return JSMN_ERROR_INVAL;
                    token->end = parser->pos + 1;
#ifdef JSMN_PARENT_LINKS
                    parser->toksuper = token->parent;
#else
                    parser->toksuper = -1;
                    for (int i = parser->toknext - 1; i >= 0; i--) {
                        token = &tokens[i];
                        if (token->start != -1 && token->end == -1) {
                            parser->toksuper = i;
                            break;
                        }
                    }
#endif
                    break;
                }
#ifdef JSMN_PARENT_LINKS
                if (token->parent == -1) return JSMN_ERROR_INVAL;
                token = &tokens[token->parent];
#else
                if (token == tokens) return JSMN_ERROR_INVAL;
                token--;
#endif
            }
            break;
        case '\"':
            r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
            if (r < 0) return r;
            count++;
            if (parser->toksuper != -1 && tokens != NULL) tokens[parser->toksuper].size++;
            break;
        case '\t': case '\r': case '\n': case ' ': break;
        case ':': parser->toksuper = parser->toknext - 1; break;
        case ',':
            if (tokens != NULL && parser->toksuper != -1 &&
                tokens[parser->toksuper].type != JSMN_ARRAY &&
                tokens[parser->toksuper].type != JSMN_OBJECT) {
#ifdef JSMN_PARENT_LINKS
                parser->toksuper = tokens[parser->toksuper].parent;
#else
                for (int i = parser->toknext - 1; i >= 0; i--) {
                    if (tokens[i].type == JSMN_ARRAY || tokens[i].type == JSMN_OBJECT) {
                        if (tokens[i].start != -1 && tokens[i].end == -1) {
                            parser->toksuper = i;
                            break;
                        }
                    }
                }
#endif
            }
            break;
        default:
            r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
            if (r < 0) return r;
            count++;
            if (parser->toksuper != -1 && tokens != NULL) tokens[parser->toksuper].size++;
            break;
        }
    }
    if (tokens != NULL) {
        for (int i = parser->toknext - 1; i >= 0; i--) {
            if (tokens[i].start != -1 && tokens[i].end == -1) return JSMN_ERROR_PART;
        }
    }
    return count;
}

#ifdef __cplusplus
}
#endif
