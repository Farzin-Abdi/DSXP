#include "textfold.h"

#include <stdbool.h>

/* The font is ASCII 32..126, so tag text has to be transliterated rather
 * than copied.
 *
 * Accented Latin-1 letters fold to their base letter, which is how
 * "Bjork" is better than "Bj?rk"; the typographic punctuation taggers
 * love folds to the ASCII equivalent.
 *
 * Arabic and Persian are romanised rather than dropped. A library of
 * Persian music tagged in Persian used to arrive as a row of question
 * marks - the artist line said "?????????" and there was no way to tell
 * one track from another. Romanised it says "Dariush", which is not the
 * script the tag was written in but is the name, and a name that can be
 * read beats a name that cannot.
 *
 * Note this is transliteration, not rendering: the letters are spelled
 * out in Latin. Showing the script itself needs a font with Arabic
 * glyphs, contextual shaping - each letter has up to four forms
 * depending on its neighbours - and right-to-left layout, none of which
 * this has.
 *
 * Anything with no spelling becomes '?' so the loss stays visible rather
 * than silently shortening the title. */
const char *TextFold_Codepoint(unsigned c, char *one)
{
    if (c >= 32 && c <= 126) { one[0] = (char)c; one[1] = '\0'; return one; }

    /* The ligatures, which are two letters wearing one codepoint.
     * Ahead of the block tables below, which hold one letter a slot and
     * would fold each of these to half of itself. */
    switch (c) {
    case 0x00DF:              return "ss";   /* the Latin-1 sharp s */
    case 0x00C6: case 0x00E6: return "ae";
    case 0x0132: case 0x0133: return "ij";
    case 0x0152: case 0x0153: return "oe";
    default: break;
    }

    static const char *kUpper = "AAAAAAECEEEEIIIIDNOOOOO*OUUUUY?s";  /* C0..DF */
    static const char *kLower = "aaaaaaeceeeeiiiidnooooo/ouuuuy?y";  /* E0..FF */
    if (c >= 0xC0 && c <= 0xDF) { one[0] = kUpper[c - 0xC0]; one[1] = '\0'; return one; }
    if (c >= 0xE0 && c <= 0xFF) { one[0] = kLower[c - 0xE0]; one[1] = '\0'; return one; }

    /* Latin Extended-A: the accented letters the rest of Europe uses.
     *
     * One entry per codepoint from U+0100 to U+017F, folded to the base
     * letter - c-acute and c-caron are both "c", the way Latin-1's
     * e-acute is already "e". Polish, Czech, Turkish, Hungarian, the
     * Baltic languages and Romanian all live in this block, and without
     * it every one of them arrived as a row of question marks.
     *
     * Generated from the Unicode decompositions rather than typed out: a
     * hundred and twenty-eight hand-counted letters is a table with a
     * silent off-by-one in it, and that off-by-one would be one wrong
     * letter in one language nobody here reads. */
    static const char kLatinA[] =
        /* 0100 */ "AaAaAaCcCcCcCcDd"
        /* 0110 */ "DdEeEeEeEeEeGgGg"
        /* 0120 */ "GgGgHhHhIiIiIiIi"
        /* 0130 */ "IiIiJjKkkLlLlLlL"
        /* 0140 */ "lLlNnNnNnnNnOoOo"
        /* 0150 */ "OoOoRrRrRrSsSsSs"
        /* 0160 */ "SsTtTtTtUuUuUuUu"
        /* 0170 */ "UuUuWwYyYZzZzZzs";
    _Static_assert(sizeof(kLatinA) == 0x80 + 1, "one entry per codepoint");

    if (c >= 0x0100 && c <= 0x017F) {
        one[0] = kLatinA[c - 0x0100]; one[1] = '\0'; return one;
    }

    /* Fullwidth forms, which are ASCII with a fixed offset.
     *
     * U+FF01 to U+FF5E are the printable ASCII range redrawn at the width
     * of a CJK character, and taggers and file names pick them up
     * constantly - a fullwidth hyphen or bracket in a track name looked
     * like a character this could not read and came out as a question
     * mark, in the middle of a name that was otherwise perfectly plain.
     * The mapping is exact: subtract the offset and it is the ASCII
     * character it was drawn from. The ideographic space goes with them
     * for the same reason. */
    if (c >= 0xFF01 && c <= 0xFF5E) {
        one[0] = (char)(c - 0xFEE0); one[1] = '\0'; return one;
    }
    if (c == 0x3000) return " ";

    /* Greek accents, folded onto the plain letter before the table is
     * consulted. A Greek name is normally written with its tonos, so
     * without this the one accented vowel in it - which is most of them -
     * would be the single character that failed. */
    switch (c) {
    case 0x0386: c = 0x0391; break;
    case 0x0388: c = 0x0395; break;
    case 0x0389: c = 0x0397; break;
    case 0x038A: case 0x03AA: c = 0x0399; break;
    case 0x038C: c = 0x039F; break;
    case 0x038E: case 0x03AB: c = 0x03A5; break;
    case 0x038F: c = 0x03A9; break;
    case 0x03AC: c = 0x03B1; break;
    case 0x03AD: c = 0x03B5; break;
    case 0x03AE: c = 0x03B7; break;
    case 0x03AF: case 0x03CA: case 0x0390: c = 0x03B9; break;
    case 0x03CB: case 0x03CD: case 0x03B0: c = 0x03C5; break;
    case 0x03CC: c = 0x03BF; break;
    case 0x03CE: c = 0x03C9; break;
    default: break;
    }

    /* Cyrillic and Greek: one table each, in lower case, and the capital
     * range capitalises the result.
     *
     * Two tables per script would be two places for "shch" to be spelled,
     * and they would drift. Capitalising instead means the letter and its
     * capital cannot disagree about anything but the first character -
     * which is the only thing that differs.
     *
     * The romanisation is the one a name is normally written with in
     * Latin: "zh" for zhe, "shch" for shcha, beta as "v" and eta as "i"
     * because that is how a Greek name sounds now. Same reasoning as the
     * Persian further down - this is not the script the tag was written
     * in, but it is the name, and a name that can be read beats one that
     * cannot. */
    {
        static const char *const kCyr[32] = {
            "a", "b", "v", "g", "d", "e", "zh", "z",
            "i", "y", "k", "l", "m", "n", "o", "p",
            "r", "s", "t", "u", "f", "kh", "ts", "ch",
            "sh", "shch", "", "y", "", "e", "yu", "ya",
        };
        /* Twenty-five, because final sigma has its own codepoint at the
         * end of the lower-case run and no capital to match. */
        static const char *const kGreek[25] = {
            "a", "v", "g", "d", "e", "z", "i", "th", "i",
            "k", "l", "m", "n", "x", "o", "p", "r", "s",
            "s", "t", "y", "f", "ch", "ps", "o",
        };

        const char *word = NULL;
        bool upper = false;

        if (c >= 0x0410 && c <= 0x042F)      { word = kCyr[c - 0x0410]; upper = true; }
        else if (c >= 0x0430 && c <= 0x044F) { word = kCyr[c - 0x0430]; }
        else if (c >= 0x0391 && c <= 0x03A9) { word = kGreek[c - 0x0391]; upper = true; }
        else if (c >= 0x03B1 && c <= 0x03C9) { word = kGreek[c - 0x03B1]; }

        if (word) {
            if (!upper || !word[0]) return word;

            size_t i = 0;
            for (; word[i] && i + 1 < TEXTFOLD_SCRATCH; i++) one[i] = word[i];
            one[i] = '\0';
            if (one[0] >= 'a' && one[0] <= 'z') one[0] = (char)(one[0] - 32);
            return one;
        }
    }

    /* Arabic-Indic and Persian digits. */
    if (c >= 0x0660 && c <= 0x0669) { one[0] = (char)('0' + (c - 0x0660)); one[1] = '\0'; return one; }
    if (c >= 0x06F0 && c <= 0x06F9) { one[0] = (char)('0' + (c - 0x06F0)); one[1] = '\0'; return one; }

    /* Harakat - the short-vowel marks - and the joiners carry no letter
     * and are dropped rather than spelled. */
    if (c >= 0x064B && c <= 0x0652) return "";
    if (c == 0x0640 || c == 0x200C || c == 0x200D || c == 0x200E || c == 0x200F) return "";

    switch (c) {
    /* Arabic and Persian letters. Where the two traditions differ the
     * Persian reading is used - "v" for waw, "q" for qaf - because that
     * is the library this was written for. */
    case 0x0621: case 0x0654: case 0x0655: return "'";
    case 0x0622: case 0x0623: case 0x0627: return "a";
    case 0x0624:                           return "o";
    case 0x0625:                           return "e";
    case 0x0626: case 0x0649: case 0x064A: case 0x06CC: return "y";
    case 0x0628:                           return "b";
    case 0x0629: case 0x0647: case 0x06C0: case 0x06D5: return "h";
    case 0x062A: case 0x0637:              return "t";
    case 0x062B: case 0x0633: case 0x0635: return "s";
    case 0x062C:                           return "j";
    case 0x062D:                           return "h";
    case 0x062E:                           return "kh";
    case 0x062F:                           return "d";
    case 0x0630: case 0x0632: case 0x0636: case 0x0638: return "z";
    case 0x0631:                           return "r";
    case 0x0634:                           return "sh";
    case 0x0639:                           return "'";
    case 0x063A:                           return "gh";
    case 0x0641:                           return "f";
    case 0x0642:                           return "q";
    case 0x0643: case 0x06A9:              return "k";
    case 0x0644:                           return "l";
    case 0x0645:                           return "m";
    case 0x0646:                           return "n";
    case 0x0648:                           return "v";
    case 0x067E:                           return "p";
    case 0x0686:                           return "ch";
    case 0x0698:                           return "zh";
    case 0x06AF:                           return "g";

    /* Arabic punctuation, which is mirrored rather than different. */
    case 0x060C: return ",";
    case 0x061B: return ";";
    case 0x061F: return "?";
    case 0x06D4: return ".";
    case 0x00AB: case 0x00BB: return "\"";

    /* Cyrillic letters outside the plain Russian run: yo and the
     * Ukrainian, Belarusian and Serbian additions. */
    case 0x0401: case 0x0451: return "yo";
    case 0x0402: case 0x0452: return "d";
    case 0x0404: case 0x0454: return "ye";
    case 0x0405: case 0x0455: return "z";
    case 0x0406: case 0x0456: case 0x0407: case 0x0457: return "i";
    case 0x0408: case 0x0458: return "j";
    case 0x0409: case 0x0459: return "lj";
    case 0x040A: case 0x045A: return "nj";
    case 0x040B: case 0x045B: return "c";
    case 0x040E: case 0x045E: return "u";
    case 0x0490: case 0x0491: return "g";

    case 0x2018: case 0x2019: case 0x201A: return "'";
    case 0x201C: case 0x201D: case 0x201E: return "\"";
    case 0x2013: case 0x2014: case 0x2015: return "-";
    case 0x2026: return ".";      /* ellipsis; three would not fit */
    case 0x00A0: return " ";
    case 0x00B7: case 0x2022: return "*";
    default: return "?";
    }
}

void TextFold_Append(char *dst, size_t cap, size_t *len, unsigned cp)
{
    char one[TEXTFOLD_SCRATCH];
    const char *sp = TextFold_Codepoint(cp, one);

    for (; *sp; sp++) {
        if (*len + 1 >= cap) return;
        /* Leading spaces and runs of them are dropped: taggers pad
         * fields and a title that starts three pixels in looks like a
         * layout bug. */
        if (*sp == ' ' && (*len == 0 || dst[*len - 1] == ' ')) continue;
        dst[(*len)++] = *sp;
        dst[*len] = '\0';
    }
}


void TextFold_Utf8(const char *in, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    if (!in) return;

    for (size_t i = 0; in[i]; ) {
        unsigned char c0 = (unsigned char)in[i];
        unsigned cp = c0;
        int extra = (c0 >= 0xF0) ? 3 : (c0 >= 0xE0) ? 2 : (c0 >= 0xC0) ? 1 : 0;

        /* A continuation byte where a lead byte should be, or a sequence
         * that runs off the end, is taken one byte at a time rather than
         * abandoned - the name still has to be readable enough to pick
         * out of a list. */
        if (extra) {
            int have = 0;
            for (int k = 0; k < extra; k++)
                if (((unsigned char)in[i + 1 + k] & 0xC0) == 0x80) have++;
                else break;

            if (have == extra) {
                cp &= (0x3F >> extra);
                for (int k = 0; k < extra; k++)
                    cp = (cp << 6) | ((unsigned char)in[i + 1 + k] & 0x3F);
                i += 1 + extra;
            } else {
                i += 1;
            }
        } else {
            i += 1;
        }

        TextFold_Append(out, cap, &len, cp);
    }

    while (len && out[len - 1] == ' ') out[--len] = '\0';
}
