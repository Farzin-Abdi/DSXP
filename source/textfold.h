/* Making text renderable by a font that only has ASCII.
 *
 * Shared, because two places need it and they must agree: ID3 tags, and
 * the names of files and folders on the card. A Persian folder name went
 * to the screen as raw UTF-8 and came out as a row of question marks -
 * the decoder had known how to transliterate that text for tags since
 * long before, and the browser simply never asked it. */
#ifndef DSXP_TEXTFOLD_H
#define DSXP_TEXTFOLD_H

#include <stddef.h>

/* Scratch the caller lends to TextFold_Codepoint.
 *
 * Big enough for the longest thing it builds there: a capitalised
 * Cyrillic shcha is "Shch", and the table it is capitalised from holds
 * lower case only. Lent rather than kept in a static, because the tags
 * are folded on the decoder's worker thread and the file names on the
 * UI thread, and one buffer between them is one name overwriting the
 * other's letters mid-word. */
#define TEXTFOLD_SCRATCH 8

/* One code point to a short ASCII string. `scratch` is at least
 * TEXTFOLD_SCRATCH bytes; the return value may point into it, and is
 * valid until the next call that borrows the same buffer. */
const char *TextFold_Codepoint(unsigned c, char *scratch);

/* Appends one code point's transliteration to `dst`, keeping `*len` in
 * step and never exceeding `cap`. */
void TextFold_Append(char *dst, size_t cap, size_t *len, unsigned cp);

/* A whole UTF-8 string, transliterated into `out`.
 *
 * Malformed input is not an error worth failing on - a filename comes
 * off a FAT volume and can be anything - so a byte that does not begin a
 * valid sequence is folded on its own, which lands it in the Latin-1
 * range and produces a letter rather than nothing. */
void TextFold_Utf8(const char *in, char *out, size_t cap);

#endif /* DSXP_TEXTFOLD_H */
