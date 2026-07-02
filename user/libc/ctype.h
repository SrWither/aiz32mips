// ctype.h — clasificación/conversión de caracteres ASCII, subconjunto
// chico de <ctype.h>: sin locale (este proyecto no tiene ninguna noción de
// eso), alcanza para lo que necesita un parser/tokenizer chico.
#ifndef AIZ_LIBC_CTYPE_H
#define AIZ_LIBC_CTYPE_H

static inline int isdigit(int c) {
    return c >= '0' && c <= '9';
}

static inline int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}

static inline int islower(int c) {
    return c >= 'a' && c <= 'z';
}

static inline int isalpha(int c) {
    return isupper(c) || islower(c);
}

static inline int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static inline int ispunct(int c) {
    return c > 32 && c < 127 && !isalnum(c);
}

static inline int toupper(int c) {
    return islower(c) ? c - ('a' - 'A') : c;
}

static inline int tolower(int c) {
    return isupper(c) ? c + ('a' - 'A') : c;
}

#endif // AIZ_LIBC_CTYPE_H
