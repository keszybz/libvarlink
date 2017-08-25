#include "scanner.h"
#include "util.h"
#include "varlink.h"

#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *error_strings[] = {
        [SCANNER_ERROR_PANIC] = "Panic",
        [SCANNER_ERROR_INTERFACE_KEYWORD_EXPECTED] = "InterfaceKeywordExpected",
        [SCANNER_ERROR_KEYWORD_EXPECTED] = "KeywordExpected",
        [SCANNER_ERROR_DUPLICATE_FIELD_NAME] = "DuplicateFieldName",
        [SCANNER_ERROR_INTERFACE_NAME_INVALID] = "InterfaceNameInvalid",
        [SCANNER_ERROR_OBJECT_EXPECTED] = "ObjectExpected",
        [SCANNER_ERROR_DUPLICATE_MEMBER_NAME] = "DuplicateMemberName",
        [SCANNER_ERROR_MEMBER_NAME_INVALID] = "MemberNameInvalid",
        [SCANNER_ERROR_UNKNOWN_TYPE] = "UnknownType",
        [SCANNER_ERROR_FIELD_NAME_INVALID] = "FieldNameInvalid",
        [SCANNER_ERROR_TYPE_NAME_INVALID] = "TypeNameInvalid",
        [SCANNER_ERROR_INVALID_CHARACTER] = "InvalidCharacter",
        [SCANNER_ERROR_OPERATOR_EXPECTED] = "OperatorExpected",
        [SCANNER_ERROR_TYPE_EXPECTED] = "TypeExpected",
        [SCANNER_ERROR_JSON_EXPECTED] = "JsonExpected",
};

const char *scanner_error_string(long error) {
        if (error == 0 || error >= (long)ARRAY_SIZE(error_strings))
                return "<invalid>";

        if (!error_strings[error])
                return "<missing>";

        return error_strings[error];
}

static long scanner_new(Scanner **scannerp, const char *string) {
        Scanner *scanner;

        scanner = calloc(1, sizeof(Scanner));
        scanner->string = string;
        scanner->p = scanner->string;
        scanner->pline = scanner->string;
        scanner->line_nr = 1;

        *scannerp = scanner;

        return 0;
}

long scanner_new_interface(Scanner **scannerp, const char *string) {
        return scanner_new(scannerp, string);
}

long scanner_new_json(Scanner **scannerp, const char *string) {
        Scanner *scanner;
        long r;

        r = scanner_new(&scanner, string);
        if (r < 0)
                return r;

        scanner->json = true;
        *scannerp = scanner;

        return 0;
}

void scanner_free(Scanner *scanner) {
        free(scanner->error.identifier);
        free(scanner);
}

void scanner_freep(Scanner **scannerp) {
        if (*scannerp)
                scanner_free(*scannerp);
}

bool scanner_error(Scanner *scanner, long error, const char *identifier) {
        if (scanner->error.no == 0) {
                scanner->error.no = error;
                scanner->error.line_nr = scanner->line_nr;
                scanner->error.pos_nr = 1 + scanner->p - scanner->pline;

                if (identifier)
                        scanner->error.identifier = strdup(identifier);
        }

        return false;
}

static const char *scanner_advance(Scanner *scanner) {
        for (;;) {
                switch (*scanner->p) {
                        case ' ':
                        case '\t':
                                scanner->p += 1;
                                break;

                        case '\n':
                                if (scanner->pline == scanner->p)
                                        scanner->last_comment_start = NULL;

                                scanner->p += 1;
                                scanner->pline = scanner->p;
                                scanner->line_nr += 1;
                                break;

                        case '#':
                                if (scanner->json)
                                        return scanner->p;

                                if (!scanner->last_comment_start)
                                        scanner->last_comment_start = scanner->p;

                                scanner->p = strchrnul(scanner->p, '\n');
                                break;

                        default:
                                return scanner->p;
                }
        }
}

char *scanner_get_last_docstring(Scanner *scanner) {
        FILE *stream = NULL;
        char *docstring = NULL;
        unsigned long size;
        const char *p;

        scanner_advance(scanner);

        if (!scanner->last_comment_start)
                return NULL;

        stream = open_memstream(&docstring, &size);

        p = scanner->last_comment_start;
        while (*p == '#') {
                const char *start = p + 1;

                if (*start == ' ')
                        start += 1;

                p = strchrnul(start, '\n');
                p += 1;

                fwrite(start, 1, p - start, stream);

                /* Skip all leading whitespace in the next line */
                while (*p == ' ' || *p == '\t')
                        p += 1;
        }

        fclose(stream);

        scanner->last_comment_start = NULL;

        return docstring;
}

char scanner_peek(Scanner *scanner) {
        scanner_advance(scanner);

        return *scanner->p;
}

static unsigned long scanner_word_len(Scanner *scanner) {
        scanner_advance(scanner);

        switch (*scanner->p) {
                case 'a' ... 'z':
                case 'A' ... 'Z':
                        break;

                default:
                        return 0;
        }

        for (unsigned long i = 1;; i += 1) {
                switch (scanner->p[i]) {
                        case '0' ... '9':
                        case 'a' ... 'z':
                        case 'A' ... 'Z':
                        case '_':
                        case '-':
                        case '.':
                                break;

                        default:
                                return i;
                }
        }
}

bool scanner_read_keyword(Scanner *scanner, const char *keyword) {
        unsigned long word_len = scanner_word_len(scanner);
        unsigned long keyword_len = strlen(keyword);

        if (word_len != keyword_len)
                return false;

        if (strncmp(scanner->p, keyword, word_len) != 0)
                return false;

        scanner->p += word_len;

        return true;
}

static bool interface_name_valid(const char *name, unsigned long len) {
        bool has_dot = false;
        bool has_alpha = false;

        if (len < 3 || len > 255)
                return false;

        if (name[0] == '.' || name[len - 1] == '.')
                return false;

        if (name[0] == '-' || name[len - 1] == '-')
                return false;

        for (unsigned long i = 0; i < len; i += 1) {
                switch (name[i]) {
                        case 'a' ... 'z':
                                has_alpha = true;
                                break;

                        case '0' ... '9':
                                break;

                        case '.':
                                if (name[i - 1] == '.')
                                        return false;

                                if (name[i - 1] == '.')
                                        return false;

                                if (!has_alpha)
                                        return false;

                                has_dot = true;
                                break;

                        case '-':
                                if (name[i - 1] == '.')
                                        return false;

                                break;

                        default:
                                return false;
                }
        }

        if (!has_dot || !has_alpha)
                return false;

        return true;
}

bool scanner_expect_interface_name(Scanner *scanner, char **namep) {
        unsigned long len = scanner_word_len(scanner);

        if (!interface_name_valid(scanner->p, len))
                return scanner_error(scanner, SCANNER_ERROR_INTERFACE_NAME_INVALID, NULL);

        *namep = strndup(scanner->p, len);
        scanner->p += len;

        return true;
}

bool scanner_expect_field_name(Scanner *scanner, char **namep) {
        unsigned long len = scanner_word_len(scanner);

        switch (*scanner->p) {
                case 'a' ... 'z':
                case 'A' ... 'Z':
                        break;

                default:
                        return scanner_error(scanner, SCANNER_ERROR_FIELD_NAME_INVALID, NULL);
        }

        for (unsigned long i = 1; i < len; i += 1) {
                switch (scanner->p[i]) {
                        case '0' ... '9':
                        case 'a' ... 'z':
                        case 'A' ... 'Z':
                        case '_':
                                break;

                        default:
                                return scanner_error(scanner, SCANNER_ERROR_FIELD_NAME_INVALID, NULL);
                }
        }

        switch (scanner->p[len - 1]) {
                case '0' ... '9':
                case 'a' ... 'z':
                case 'A' ... 'Z':
                        break;

                default:
                        return scanner_error(scanner, SCANNER_ERROR_FIELD_NAME_INVALID, NULL);
        }

        *namep = strndup(scanner->p, len);
        scanner->p += len;

        return true;
}

static bool member_name_valid(const char *member, unsigned long len) {
        switch (*member) {
                case 'A' ... 'Z':
                        break;

                default:
                        return false;
        }

        for (unsigned long i = 1; i < len; i += 1) {
                switch (member[i]) {
                        case '0' ... '9':
                        case 'a' ... 'z':
                        case 'A' ... 'Z':
                                break;

                        default:
                                return false;
                }
        }

        return true;
}

bool scanner_expect_member_name(Scanner *scanner, char **namep) {
        unsigned long len = scanner_word_len(scanner);

        if (!member_name_valid(scanner->p, len))
                return scanner_error(scanner, SCANNER_ERROR_MEMBER_NAME_INVALID, NULL);

        *namep = strndup(scanner->p, len);
        scanner->p += len;

        return true;
}

bool scanner_expect_type_name(Scanner *scanner, char **namep) {
        unsigned long len = scanner_word_len(scanner);
        unsigned long interface_len = 0;
        const char *member = NULL;
        unsigned long member_len = 0;

        if (member_name_valid(scanner->p, len)) {
                *namep = strndup(scanner->p, len);
                scanner->p += len;
                return true;
        }

        if (len < 3)
                return scanner_error(scanner, SCANNER_ERROR_TYPE_NAME_INVALID, NULL);

        for (unsigned long i = 0; i < len; i += 1) {
                if (scanner->p[i] >= 'A' && scanner->p[i] <= 'Z') {
                        if (scanner->p[i - 1] != '.')
                                return scanner_error(scanner, SCANNER_ERROR_TYPE_NAME_INVALID, NULL);

                        interface_len = i - 1;
                        member = scanner->p + i;
                        member_len = len - i;
                        break;
                }
        }

        if (!interface_name_valid(scanner->p, interface_len))
                return scanner_error(scanner, SCANNER_ERROR_TYPE_NAME_INVALID, NULL);

        if (!member_name_valid(member, member_len))
                return scanner_error(scanner, SCANNER_ERROR_TYPE_NAME_INVALID, NULL);

        *namep = strndup(scanner->p, len);
        scanner->p += len;

        return true;
}

static bool unhex(char d, uint8_t *valuep) {
        switch (d) {
                case '0' ... '9':
                        *valuep = d - '0';
                        return true;

                case 'a' ... 'f':
                        *valuep = d - 'a' + 0x0a;
                        return true;

                case 'A' ... 'F':
                        *valuep = d - 'A' + 0x0a;
                        return true;

                default:
                        return false;
        }
}

static bool read_unicode_char(const char *p, FILE *stream) {
        uint8_t digits[4];
        uint16_t cp;

        for (unsigned long i = 0; i < 4; i += 1)
                if (p[i] == '\0' || !unhex(p[i], &digits[i]))
                        return false;

        cp = digits[0] << 12 | digits[1] << 8 | digits[2] << 4 | digits[3];

        if (cp <= 0x007f) {
                fprintf(stream, "%c", (char)cp);

        } else if (cp <= 0x07ff) {
                fprintf(stream, "%c", (char)(0xc0 | (cp >> 6)));
                fprintf(stream, "%c", (char)(0x80 | (cp & 0x3f)));
        }

        else {
                fprintf(stream, "%c", (char)(0xe0 | (cp >> 12)));
                fprintf(stream, "%c", (char)(0x80 | ((cp >> 6) & 0x3f)));
                fprintf(stream, "%c", (char)(0x80 | (cp & 0x3f)));
        }

        return true;
}


bool scanner_expect_json_string(Scanner *scanner, char **stringp) {
        _cleanup_(freep) char *string = NULL;
        _cleanup_(fclosep) FILE *stream = NULL;
        unsigned long size;
        const char *p;

        p = scanner_advance(scanner);

        if (*p != '"')
                return false;

        p += 1;

        stream = open_memstream(&string, &size);

        for (;;) {
                if (*p == '\0')
                        return false;

                if (*p == '"') {
                        p += 1;
                        break;
                }

                if (*p == '\\') {
                        p += 1;
                        switch (*p) {
                                case '"':
                                        fprintf(stream, "\"");
                                        break;

                                case '\\':
                                        fprintf(stream, "\\");
                                        break;

                                case '/':
                                        fprintf(stream, "/");
                                        break;

                                case 'b':
                                        fprintf(stream, "\b");
                                        break;

                                case 'f':
                                        fprintf(stream, "\f");
                                        break;

                                case 'n':
                                        fprintf(stream, "\n");
                                        break;

                                case 'r':
                                        fprintf(stream, "\r");
                                        break;

                                case 't':
                                        fprintf(stream, "\t");
                                        break;

                                case 'u':
                                        if (!read_unicode_char(p + 1, stream))
                                                return scanner_error(scanner, SCANNER_ERROR_INVALID_CHARACTER, NULL);

                                        p += 4;
                                        break;

                                default:
                                        return scanner_error(scanner, SCANNER_ERROR_INVALID_CHARACTER, NULL);
                        }
                } else
                        fprintf(stream, "%c", *p);

                p += 1;
        }

        fclose(stream);
        stream = NULL;

        if (stringp) {
                *stringp = string;
                string = NULL;
        }

        scanner->p = p;

        return true;
}

bool scanner_read_number(Scanner *scanner, ScannerNumber *numberp) {
        ScannerNumber number = {};
        char *end;

        scanner_advance(scanner);

        number.i = strtol(scanner->p, &end, 10);
        if (end == scanner->p)
                return false;

        if (*end == '.' || *end == 'e' || *end == 'E') {
                locale_t loc;

                loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t) 0);

                number.is_double = true;
                number.d = strtod(scanner->p, &end);

                freelocale(loc);
        }

        scanner->p = end;

        if (numberp)
                *numberp = number;

        return true;
}

bool scanner_read_uint(Scanner *scanner, uint64_t *uintp) {
        uint64_t u;

        scanner_advance(scanner);

        /*
         * Don't allow leading 0s and negative values (strtoul converts
         * those to positives)
         */
        if (*scanner->p < '1' || *scanner->p > '9')
                return false;

        u = strtoul(scanner->p, (char **)&scanner->p, 10);

        if (uintp)
                *uintp = u;

        return true;
}

bool scanner_expect_operator(Scanner *scanner, const char *op) {
        unsigned long length = strlen(op);

        scanner_advance(scanner);

        if (strncmp(scanner->p, op, length) != 0)
                return scanner_error(scanner, SCANNER_ERROR_OPERATOR_EXPECTED, op);

        scanner->p += length;

        return true;
}
