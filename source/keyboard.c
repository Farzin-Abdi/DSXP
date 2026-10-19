#include "keyboard.h"

#include <string.h>
#include <strings.h>   /* strcasecmp, for the .m3u the picker hides */
#include <stdio.h>

#include "gfx.h"
#include "skin.h"
#include "qr_data.h"

/* ---- layout -------------------------------------------------------
 *
 * Ten keys across, which at 256 pixels gives 25 each - about the
 * narrowest a stylus lands on reliably, and exactly wide enough for a
 * QWERTY row without splitting it. The rows are the familiar ones rather
 * than alphabetical: people hunt for letters by shape of the layout, and
 * an alphabetical grid is only easier for someone who has never seen a
 * keyboard. */
#define KB_COLS   10
#define KB_KEY_W  24
#define KB_KEY_H  24
#define KB_X      3
#define KB_Y      50
#define KB_GAP    1

/* Ten 25-wide keys need 259 pixels and the screen has 256, so the right
 * hand column used to be drawn six pixels off the edge - "0" and "P"
 * arrived sliced in half and the key beside Cancel could not be tapped
 * at its right end. Twenty-four fits with room to spare. */
_Static_assert(KB_X + KB_COLS * KB_KEY_W + (KB_COLS - 1) * KB_GAP <= 256,
               "the key grid runs off the screen");

#define FIELD_X   8
#define FIELD_Y   24
#define FIELD_W   240
#define FIELD_H   18

/* Ten keys on every row.
 *
 * The bottom two rows of a QWERTY layout are nine and seven letters, and
 * centring them left a ragged grid with three empty slots. Those slots
 * now carry the punctuation a filename actually needs - without an
 * apostrophe or a hyphen there is no way to type "Chill-out" or "80's",
 * and a keyboard that cannot spell the name you want is not much of a
 * keyboard. They are not case-sensitive, so shift leaves them alone. */
static const char *const kRows[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL'",
    "ZXCVBNM-_.",
};

static int RowOffset(int row) { (void)row; return 0; }

/* The bottom strip: shift, space, backspace, and the two answers. */
#define BOT_Y   (KB_Y + 4 * (KB_KEY_H + KB_GAP) + 6)
#define BOT_H   24

typedef struct { int x, w; const char *label; int id; } BotKey;

enum { K_SHIFT = -2, K_SPACE = -3, K_BACK = -4, K_OK = -5, K_CANCEL = -6,
       K_TAB_SWAP = -7,        /* the support screen changed currency */
       K_ENTRY_OPEN = -8, K_ENTRY_HELP = -9, K_ENTRY_SUPPORT = -10,
 };

/* Widths measured against the labels rather than guessed. "Cancel" and
 * "Delete" are both 38 pixels, so the two rightmost keys need enough
 * padding for them - the first cut had Cancel running off the screen and
 * Delete spilling into it. */
static const BotKey kBottom[] = {
    {   2, 38, "abc",    K_SHIFT  },
    {  42, 62, "space",  K_SPACE  },
    { 106, 38, "del",    K_BACK   },
    { 146, 52, "OK",     K_OK     },
    { 200, 54, "Cancel", K_CANCEL },
};
#define BOT_COUNT ((int)(sizeof(kBottom) / sizeof(kBottom[0])))

/* ---- state -------------------------------------------------------- */

static KeyboardMode s_mode = KB_IDLE;
static char  s_text[64];
static size_t s_max = sizeof(s_text);
static char  s_prompt[48];
static char  s_subject[64];
static bool  s_lower = false;

/* True while the case is the keyboard's idea rather than the user's.
 *
 * A name starts with a capital and carries on in lower case, which is
 * what everyone types and what every phone does - so the first letter is
 * capital by default and the shift drops itself afterwards. Pressing
 * shift by hand clears this: an explicit choice is a lock, because a
 * shift that undoes itself when you meant to hold it is worse than one
 * that does nothing. */
static bool  s_autoShift = true;

/* Set once the user presses shift themselves. After that the keyboard
 * stops having opinions about case - restoring the capital on an empty
 * field would be overriding a choice that was made deliberately. */
static bool  s_manualShift = false;

static bool s_accepted = false, s_cancelled = false;

/* What the yes button says, and whether the page warns. Both belong to
 * the question being asked rather than to the modal. */
static char s_verb[16] = "OK";
static bool s_destructive = false;

/* True while the d-pad is what is driving the page.
 *
 * The focus ring is for the d-pad, and drawing it during a stylus
 * session put a hard outline around the last key tapped and left it
 * there - a keyboard that looks stuck on one letter, and on the picker
 * an outline around Cancel while A chose the highlighted row instead.
 * Nobody using a stylus needs to be told where the cursor is; the press
 * flash already says what was hit. */
static bool s_padFocus = false;

/* Picker state. The names are borrowed - they live in the browser's
 * entry table, which outlives the modal. */
static const char *const *s_names = NULL;
static int   s_nameCount = 0;
static const char *const *s_extras = NULL;
static int   s_extraCount = 0;
static int   s_pickSel = 0, s_pickTop = 0;

/* The rows start under the heading rule rather than a third of the way
 * down the page. At 50 there were five rows and forty empty pixels above
 * them - a panel that looked like it had been dropped rather than laid
 * out, and one row fewer than fits. */
#define PICK_Y      32
#define PICK_ROW_H  20
#define PICK_ROWS   6
#define PICK_X      6
#define PICK_W      236   /* clear of the rail, which is where the file list's is */

/* Where the heading rule goes, on both pages that have a prompt above a
 * body: it is the one line that says the prompt is a heading and not the
 * first row of the thing underneath it. */
#define HEAD_RULE_Y 24

/* The rail, at the file list's x and width so the two match. */
#define PICK_RAIL_X 246
#define PICK_RAIL_W 6

#define PICK_ICON_W 9
#define PICK_ICON_H 9

_Static_assert(PICK_Y + PICK_ROWS * PICK_ROW_H <= BOT_Y,
               "the picker rows run into the answer strip");
_Static_assert(PICK_X + PICK_W <= PICK_RAIL_X,
               "the picker rows run under the rail");

/* Which key the d-pad is on: row 0..3 in the grid, row 4 is the bottom
 * strip. */
static int s_curRow = 1, s_curCol = 0;

/* The key under the stylus, so a press is visible. Without it a tap on a
 * dead spot and a tap on a key look identical, and the only way to tell
 * is whether a letter appeared.
 *
 * It lights for a moment and then goes out on its own. It used to stay
 * lit until the next tap, which left the last letter typed sitting in
 * the accent colour under the stylus - a key that looks held down when
 * nothing is touching it, and on the last press before Done, a keyboard
 * that appeared to be stuck on one key. A real press is over in well
 * under a tenth of a second, so the mark is timed to the press rather
 * than left behind by it.
 *
 * Frames, not a lift: the touchscreen reports a release a frame or two
 * after the finger goes, which is quick enough that the flash never gets
 * seen at all. */
#define PRESS_FRAMES 7
static int s_pressedId = 0;
static int s_pressLeft = 0;

/* Caret blink. A solid bar reads as part of the text - a name ending in
 * "l" and one with the caret after it look the same - and a field with
 * nothing flashing in it does not look like somewhere to type. */
static int  s_blink = 0;
static bool s_caretOn = true;

/* Common to every opener: a fresh page has nothing accepted, nothing
 * cancelled, and no key still lit from the page before it. */
static void OpenReset(void)
{
    s_accepted = s_cancelled = false;
    s_pressedId = 0;
    s_pressLeft = 0;
    s_padFocus = false;
}

void Keyboard_OpenText(const char *prompt, const char *initial, size_t maxLen)
{
    s_mode = KB_TYPING;
    s_max = (maxLen && maxLen < sizeof(s_text)) ? maxLen : sizeof(s_text);

    snprintf(s_prompt, sizeof(s_prompt), "%s", prompt ? prompt : "");
    snprintf(s_text, s_max, "%s", initial ? initial : "");
    s_subject[0] = '\0';
    s_lower = false;
    s_autoShift = true;
    s_manualShift = false;
    s_curRow = 1; s_curCol = 0;
    OpenReset();
}

void Keyboard_OpenConfirm(const char *prompt, const char *subject,
                          const char *verb, bool destructive)
{
    s_mode = KB_CONFIRM;
    snprintf(s_prompt, sizeof(s_prompt), "%s", prompt ? prompt : "");
    snprintf(s_subject, sizeof(s_subject), "%s", subject ? subject : "");
    snprintf(s_verb, sizeof(s_verb), "%s", (verb && verb[0]) ? verb : "OK");
    s_destructive = destructive;
    s_text[0] = '\0';

    s_curRow = 4; s_curCol = 3;          /* on the yes button */
    OpenReset();
}

/* Which address the support screen is showing. */
static int s_coin = 0;

/* Where Close goes. Support and Help are reachable both from the start
 * page and (for Support) from the titlebar, and "back" means different
 * things in the two cases: returning to the hub you came from, or leaving
 * the modal entirely. Remembering which is how the same Close button does
 * the right thing without either caller having to know about the other. */
static KeyboardMode s_return = KB_IDLE;

/* The start page. One major action and two minor ones, stacked rather
 * than in a row: a full-width target is the easiest thing to hit with a
 * thumb or a stylus, and the size difference is what says which button is
 * the one you came here to press. */
#define ENTRY_MAIN_Y  92
#define ENTRY_MAIN_H  36
#define ENTRY_ALT_Y   138
#define ENTRY_ALT_H   24
#define ENTRY_ALT2_Y  166
#define ENTRY_BTN_X   24
#define ENTRY_BTN_W   208
_Static_assert(ENTRY_ALT2_Y + ENTRY_ALT_H <= 192,
               "the start page buttons run off the bottom of the screen");

#define HELP_Y        24    /* first line of the controls table */

/* Ten, not eleven. Thirteen rows at eleven ran two pixels past what the
 * Back button leaves, and the assert below was written against a count
 * of twelve so it said nothing - the last binding was drawn under the
 * button. Two pixels between rows is close, but this is a table read a
 * line at a time, not prose. */
#define HELP_ROW_H    10
#define HELP_SECTION_GAP 5    /* air before each heading but the first */
#define HELP_BACK_Y   170
#define HELP_BACK_H   18


/* The support screen sets its own vertical rhythm: the code has to be as
 * big as it can be, and everything else fits around it. */
#define SUPPORT_HEAD_Y   11
#define SUPPORT_HEAD_SZ  6     /* quarters - one and a half times normal */
#define SUPPORT_RULE_Y   27

/* The code, and every dimension of it derived rather than written twice.
 *
 * It got smaller by dropping a QR version - twenty-nine modules instead
 * of thirty-three, still nine bytes clear of the longest address - and
 * not by thinning the quiet zone or the modules, which are the two
 * things a scanner actually needs. Whitespace on the page is worth
 * having; it is not worth a code that photographs badly.
 *
 * One gap serves above the code, below it and under the address, so the
 * three read as one rhythm rather than three numbers that happen to be
 * close, and the button's position falls out of the stack rather than
 * being placed against the bottom edge and hoped for. */
#define SUPPORT_QR_SCALE 3
#define SUPPORT_QR_QUIET 2
#define SUPPORT_QR_SIDE  ((QR_SIZE + 2 * SUPPORT_QR_QUIET) * SUPPORT_QR_SCALE)
#define SUPPORT_GAP      8
#define SUPPORT_QR_Y     (SUPPORT_RULE_Y + SUPPORT_GAP)
#define SUPPORT_ADDR_Y   (SUPPORT_QR_Y + SUPPORT_QR_SIDE + SUPPORT_GAP)
#define SUPPORT_ADDR_H   17    /* two lines, the second nine rows below */

/* The switch is a column down the left rather than a strip across the
 * top. Four tabs sharing 256 pixels gave each of them sixty, which is
 * why the labels were tickers; a column has room for the names.
 *
 * It is taller than the code beside it rather than centred against it.
 * That was the right rule when the column was four short rows and the
 * code was the tall thing on the page; with five rows and full names the
 * column is the tall thing, and pinning it to the code's height only
 * squeezed the rows. It spans the page instead - clear of the rule above
 * and of the button below - and the row height falls out of that. */
#define SUPPORT_PANEL_X  4
#define SUPPORT_PANEL_W  84
#define SUPPORT_PANEL_Y  32
/* Seventeen, not twenty.
 *
 * The column spans from the rule to the Close button and the rows
 * divide what is between them, so the height is a consequence of how
 * many there are rather than a choice. Seven rows at twenty ran 17
 * pixels past the button; the layout test below says so in those terms,
 * which is how this was caught rather than seen. An eleven-pixel mark
 * and a nine-pixel line still sit in seventeen with room either side. */
#define SUPPORT_ROW_H    17
#define SUPPORT_ROW_GAP  2
#define SUPPORT_NIB_W    5     /* the selected row's point, aimed at the code */
#define SUPPORT_PANEL_H  (COIN_COUNT * SUPPORT_ROW_H + \
                          (COIN_COUNT - 1) * SUPPORT_ROW_GAP)
#define SUPPORT_ROW_Y(i) \
    (SUPPORT_PANEL_Y + (i) * (SUPPORT_ROW_H + SUPPORT_ROW_GAP))

/* One axis for the code, the address under it and the button under that:
 * the centre of what the column leaves.
 *
 * It used to be halfway between that and the centre of the screen, a
 * lean rather than a commitment, because a narrow column left sixty
 * empty pixels on the right and centring in the remainder looked like
 * the block had slid off the page. A column this wide takes that space
 * up, so the remainder is worth centring in and the two sides of the
 * page carry about the same weight. */
#define SUPPORT_COL_RIGHT (SUPPORT_PANEL_X + SUPPORT_PANEL_W + SUPPORT_NIB_W)
#define SUPPORT_AXIS      ((SUPPORT_COL_RIGHT + 256) / 2)
#define SUPPORT_QR_X      (SUPPORT_AXIS - SUPPORT_QR_SIDE / 2)

/* Under the address by the same gap as everything else, which also puts
 * it clear of the bottom edge. Written out it sat two pixels off that
 * edge - a page that has run out rather than ended, and a last row of
 * button inside the tolerance of the panel that has to detect it. */
#define SUPPORT_CLOSE_W 80
#define SUPPORT_CLOSE_X (SUPPORT_AXIS - SUPPORT_CLOSE_W / 2)
#define SUPPORT_CLOSE_Y (SUPPORT_ADDR_Y + SUPPORT_ADDR_H + SUPPORT_GAP)
#define SUPPORT_CLOSE_H 16

/* Currency marks, seven pixels wide and eleven tall: a bitcoin B with its
 * two strokes, and a Tether T struck through the stem. The strike runs the
 * full width: a first attempt had it protrude a single pixel either side of
 * the stem, which at this size disappeared and left a plain T. The top bar
 * is two rows and the strike one, so the two never read as the same mark. */
#define COIN_ICON_W 7
#define COIN_ICON_H 11
static const uint8_t kIconBTC[COIN_ICON_H] = {
    0x48,  /* .#..#.. */
    0x48,  /* .#..#.. */
    0xFC,  /* ######. */
    0xC4,  /* ##...#. */
    0xC4,  /* ##...#. */
    0xFC,  /* ######. */
    0xC4,  /* ##...#. */
    0xC4,  /* ##...#. */
    0xFC,  /* ######. */
    0x48,  /* .#..#.. */
    0x48,  /* .#..#.. */
};
static const uint8_t kIconUSDT[COIN_ICON_H] = {
    0xFE,  /* ####### */
    0xFE,  /* ####### */
    0x38,  /* ..###.. */
    0x38,  /* ..###.. */
    0xFE,  /* ####### */
    0x38,  /* ..###.. */
    0x38,  /* ..###.. */
    0x38,  /* ..###.. */
    0x38,  /* ..###.. */
    0x38,  /* ..###.. */
    0x38,  /* ..###.. */
};

/* An ether diamond - the upper solid and the lower chevron under it -
 * and Solana's three slanted bars. Both drawn at the same seven by eleven
 * as the other two so the row of tabs stays even. */
static const uint8_t kIconETH[COIN_ICON_H] = {
    0x10,  /* ...#... */
    0x38,  /* ..###.. */
    0x7C,  /* .#####. */
    0xFE,  /* ####### */
    0x7C,  /* .#####. */
    0x38,  /* ..###.. */
    0x10,  /* ...#... */
    0x00,  /* ....... */
    0x7C,  /* .#####. */
    0x38,  /* ..###.. */
    0x10,  /* ...#... */
};
static const uint8_t kIconSOL[COIN_ICON_H] = {
    0x00,  /* ....... */
    0x3E,  /* ..##### */
    0x7C,  /* .#####. */
    0x00,  /* ....... */
    0x7C,  /* .#####. */
    0x3E,  /* ..##### */
    0x00,  /* ....... */
    0x3E,  /* ..##### */
    0x7C,  /* .#####. */
    0x00,  /* ....... */
    0x00,  /* ....... */
};

/* Two branches meeting in one, which is the shape a repository has and
 * the one thing at seven pixels that says version control - the cat is
 * all in its outline and there is no outline at this size. */
static const uint8_t kIconRepo[COIN_ICON_H] = {
    0x6C,  /* .##.##. */
    0x6C,  /* .##.##. */
    0x28,  /* ..#.#.. */
    0x38,  /* ..###.. */
    0x10,  /* ...#... */
    0x10,  /* ...#... */
    0x10,  /* ...#... */
    0x10,  /* ...#... */
    0x38,  /* ..###.. */
    0x38,  /* ..###.. */
    0x38,  /* ..###.. */
};

/* A camera: the rounded body, the lens, and the small lamp at its
 * shoulder. Instagram's own mark is exactly that and nothing else, which
 * is what makes it survive being drawn seven pixels wide. */
static const uint8_t kIconInsta[COIN_ICON_H] = {
    0x7C,  /* .#####. */
    0x82,  /* #.....# */
    0x86,  /* #....## */
    0x82,  /* #.....# */
    0x92,  /* #..#..# */
    0xAA,  /* #.#.#.# */
    0x92,  /* #..#..# */
    0x82,  /* #.....# */
    0x82,  /* #.....# */
    0x7C,  /* .#####. */
    0x00,  /* ....... */
};

/* An envelope. The flap is two diagonals meeting in the middle, which
 * is what stops it reading as a plain rectangle at seven pixels - a
 * first attempt without it was indistinguishable from a blank card. */
static const uint8_t kIconMail[COIN_ICON_H] = {
    0x00,  /* ....... */
    0x00,  /* ....... */
    0xFE,  /* ####### */
    0xC6,  /* ##...## */
    0xAA,  /* #.#.#.# */
    0x92,  /* #..#..# */
    0x82,  /* #.....# */
    0x82,  /* #.....# */
    0xFE,  /* ####### */
    0x00,  /* ....... */
    0x00,  /* ....... */
};

/* A heart for the thank-you line. Eleven wide and nine tall, sized to
 * the line it sits beside rather than to the currency marks - that line
 * is set half again as large as the body text, and a seven-pixel mark
 * next to it read as an afterthought. */
#define HEART_W 11
#define HEART_H 9
static const uint16_t kIconHeart[HEART_H] = {
    0x71C0,  /* .###...###. */
    0xFBE0,  /* #####.##### */
    0xFFE0,  /* ########### */
    0xFFE0,  /* ########### */
    0x7FC0,  /* .#########. */
    0x3F80,  /* ..#######.. */
    0x1F00,  /* ...#####... */
    0x0E00,  /* ....###.... */
    0x0400,  /* .....#..... */
};

/* Names, not tickers. The column is wide enough for the longest of them
 * now, and "Ethereum" tells you what the code is for in a way that ETH
 * only does if you already knew.
 *
 * The repository is here rather than on the help page because this is
 * the page with the machinery for a link - a code to point a camera at
 * and the text underneath for when that fails - and a URL is no
 * different from an address in that respect. */
static const struct {
    const char *label, *text;
    const uint8_t *bits, *icon;
} kCoins[] = {
    { "Bitcoin",  QR_BTC_TEXT,  g_qrBTC,  kIconBTC  },
    /* The network is part of the name here, not decoration. This
     * address is on Tron, and USDT sent to it from an Ethereum wallet
     * is gone - the two chains share a token name and nothing else. Ten
     * characters is what the panel holds, which "Tether TRX" fits and
     * "Tether (Tron)" does not. */
    { "Tether TRX", QR_USDT_TEXT, g_qrUSDT, kIconUSDT },
    { "Ethereum", QR_ETH_TEXT,  g_qrETH,  kIconETH  },
    { "Solana",   QR_SOL_TEXT,  g_qrSOL,  kIconSOL  },
    { "GitHub",   QR_REPO_TEXT, g_qrREPO, kIconRepo },
    { "Instagram", QR_IG_TEXT, g_qrIG,   kIconInsta },
    { "Email",    QR_MAIL_TEXT, g_qrMAIL, kIconMail },
};
#define COIN_COUNT ((int)(sizeof(kCoins) / sizeof(kCoins[0])))

/* The column grows with the table, and the page does not. Rows are laid
 * out from the top, so one too many simply runs past the Close button
 * and off the bottom - no warning, no wrap, and the hit test would
 * still answer for a row nobody can see. Adding a row means picking a
 * row height that fits, and this is what refuses to build until one
 * has been. */
_Static_assert(SUPPORT_PANEL_Y + SUPPORT_PANEL_H < SUPPORT_CLOSE_Y,
               "the support column has more rows than fit above Close - "
               "lower SUPPORT_ROW_H");

void Keyboard_OpenEntry(void)
{
    s_mode = KB_ENTRY;
    s_prompt[0] = '\0';
    s_return = KB_IDLE;
    s_curRow = 0;                    /* on Open, the one you usually want */
    OpenReset();
}

void Keyboard_OpenHelp(void)
{
    KeyboardMode from = s_mode;
    s_mode = KB_HELP;
    s_prompt[0] = '\0';
    s_return = (from == KB_ENTRY) ? KB_ENTRY : KB_IDLE;
    s_curRow = 0;
    OpenReset();
}

void Keyboard_OpenSupport(void)
{
    KeyboardMode from = s_mode;
    s_return = (from == KB_ENTRY) ? KB_ENTRY : KB_IDLE;
    s_mode = KB_SUPPORT;
    s_prompt[0] = '\0';
    s_coin = 0;
    s_curRow = 4; s_curCol = 4;      /* on Cancel, which here means Close */
    OpenReset();
}

void Keyboard_OpenPick(const char *prompt,
                       const char *const *extras, int extraCount,
                       const char *const *names, int count)
{
    s_mode = KB_PICK;
    snprintf(s_prompt, sizeof(s_prompt), "%s", prompt ? prompt : "");

    s_extras = extras;
    s_extraCount = extraCount;
    s_names = names;
    s_nameCount = count;
    s_pickSel = 0;
    s_pickTop = 0;
    /* The list carries the focus here, so the strip cursor is parked on
     * the button A actually presses. Left where the last page had it, it
     * sat on Cancel while A chose the highlighted row - a cursor
     * pointing at one answer and a button giving the other. */
    s_curRow = 4; s_curCol = 3;
    OpenReset();
}

int Keyboard_PickedRow(void) { return s_pickSel; }

static int PickRows(void) { return s_extraCount + s_nameCount; }

KeyboardMode Keyboard_Mode(void) { return s_mode; }

bool Keyboard_Tick(void)
{
    /* The only page here that is not static. It is showing registers that
     * change while it is on screen - that is the entire point of it - so
     * it repaints on a timer rather than on input. */



    /* The press flash runs out here rather than in the touch handler:
     * every page with keys on it wants it, and a lift is not reported
     * reliably enough to hang it on. */
    bool changed = false;
    if (s_pressLeft > 0 && --s_pressLeft == 0) {
        s_pressedId = 0;
        changed = true;
    }

    if (s_mode != KB_TYPING) return changed;

    /* Twenty frames on, twenty off: a third of a second each way, which
     * is fast enough to notice and slow enough not to nag. */
    if (++s_blink < 20) return changed;
    s_blink = 0;
    s_caretOn = !s_caretOn;
    return true;
}

void Keyboard_Close(void)
{
    s_mode = KB_IDLE;
    s_return = KB_IDLE;
    OpenReset();
}

bool Keyboard_TakeAccepted(const char **outText)
{
    if (!s_accepted) return false;
    s_accepted = false;
    if (outText) *outText = s_text;
    return true;
}

bool Keyboard_TakeCancelled(void)
{
    if (!s_cancelled) return false;
    s_cancelled = false;
    return true;
}

/* ---- editing ------------------------------------------------------ */

/* The case a letter should actually be typed in. */
static char CasedChar(char ch)
{
    if (ch < 'A' || ch > 'Z') return ch;
    return s_lower ? (char)(ch + 32) : ch;
}

static void Append(char c)
{
    s_blink = 0; s_caretOn = true;

    size_t n = strlen(s_text);
    if (n + 1 >= s_max) return;
    s_text[n] = c;
    s_text[n + 1] = '\0';

    /* The automatic capital lasts exactly one letter. Digits and
     * punctuation leave it alone, so typing "80" still gives "80's" the
     * capital it was holding for the first letter. */
    if (s_autoShift && c >= 'A' && c <= 'Z') {
        s_lower = true;
        s_autoShift = false;
    }
}

static void Backspace(void)
{
    s_blink = 0; s_caretOn = true;

    size_t n = strlen(s_text);
    if (n) s_text[n - 1] = '\0';

    /* Back to an empty field is back to the start of a name, so the
     * capital comes back with it - unless the user has taken the case
     * into their own hands. */
    if (s_text[0] == '\0' && !s_manualShift) {
        s_lower = false;
        s_autoShift = true;
    }
}

/* Rejected outright rather than substituted: a name silently altered on
 * the way to the card is a file the user cannot find again. */
static bool IsAllowed(char c)
{
    return !(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
             c == '"' || c == '<' || c == '>' || c == '|');
}

static bool Activate(int id)
{
    switch (id) {
        case K_SHIFT:  s_lower = !s_lower; s_autoShift = false;
                       s_manualShift = true; return true;
        case K_SPACE:  Append(' ');        return true;
        case K_BACK:   Backspace();        return true;

        case K_OK:
            /* An empty name is not a name. Refusing is better than
             * inventing one, which would be a file the user did not ask
             * for and cannot predict. */
            if (s_mode == KB_TYPING && s_text[0] == '\0') return true;
            s_mode = KB_IDLE;
            s_accepted = true;
            return true;

        case K_CANCEL:
            /* Leaving the scanner stops it. Otherwise it carries on
             * reading every register on every frame for the rest of the
             * session, for a page nobody is looking at. */
            if (s_return == KB_ENTRY) { Keyboard_OpenEntry(); return true; }
            s_mode = KB_IDLE;
            s_cancelled = true;
            return true;

        case K_ENTRY_OPEN:
            s_mode = KB_IDLE;
            s_cancelled = true;      /* nothing was chosen, the app just starts */
            return true;



        case K_ENTRY_HELP:    Keyboard_OpenHelp();    return true;
        case K_ENTRY_SUPPORT: Keyboard_OpenSupport(); return true;

        case K_TAB_SWAP: return true;

        default:
            if (id >= 32 && IsAllowed((char)id)) { Append((char)id); return true; }
            return false;
    }
}

/* ---- input -------------------------------------------------------- */

/* Where a hit landed, so a tap can move the focus ring with it. */
static int s_hitRow = -1, s_hitCol = -1;

static bool KeyAt(int px, int py, int *outId)
{
    s_hitRow = s_hitCol = -1;

    if (s_mode == KB_TYPING) {
        for (int r = 0; r < 4; r++) {
            int y = KB_Y + r * (KB_KEY_H + KB_GAP);
            if (py < y || py >= y + KB_KEY_H) continue;

            int len = (int)strlen(kRows[r]);
            int x0 = KB_X + RowOffset(r);
            for (int c = 0; c < len; c++) {
                int x = x0 + c * (KB_KEY_W + KB_GAP);
                if (px >= x && px < x + KB_KEY_W) {
                    *outId = CasedChar(kRows[r][c]);
                    s_hitRow = r; s_hitCol = c;
                    return true;
                }
            }
        }
    }

    if (s_mode == KB_ENTRY) {
        static const struct { int y, h, id; } kEntry[3] = {
            { ENTRY_MAIN_Y, ENTRY_MAIN_H, K_ENTRY_OPEN    },
            { ENTRY_ALT_Y,  ENTRY_ALT_H,  K_ENTRY_HELP    },
            { ENTRY_ALT2_Y, ENTRY_ALT_H,  K_ENTRY_SUPPORT },
        };
        for (int i = 0; i < 3; i++) {
            /* The gaps between the buttons are live too, split between
             * the neighbours: on the start page there is nothing else a
             * tap in that column could have meant. */
            int top = (i == 0) ? 0 : (kEntry[i - 1].y + kEntry[i - 1].h
                                      + kEntry[i].y) / 2;
            int bot = (i == 2) ? 192 : (kEntry[i].y + kEntry[i].h
                                        + kEntry[i + 1].y) / 2;
            if (py >= top && py < bot && py >= ENTRY_MAIN_Y - 8) {
                *outId = kEntry[i].id;
                s_hitRow = i; s_hitCol = 0;
                return true;
            }
        }
        return false;
    }

    if (s_mode == KB_HELP) {
        if (py >= HELP_BACK_Y - 6) { *outId = K_CANCEL; return true; }
        return false;
    }



    if (s_mode == KB_SUPPORT) {
        /* The live area for the currency switch.
         *
         * It has to be derived from where the panel is actually drawn,
         * and once it was not: the tabs moved during a layout fix while
         * the hit test kept the rows it had been written with, so of a
         * fifteen-pixel tab only the bottom five selected anything and
         * the rest of the live pixels sat in empty space underneath -
         * which is a switch that feels broken rather than one that is.
         *
         * The area is deliberately larger than the rows and has no
         * gutters: every pixel of it belongs to one currency, so a tap
         * landing between two rows picks the nearer instead of nothing.
         * The split is by COIN_COUNT, so it follows the table. */
        if (px >= SUPPORT_PANEL_X - 5 &&
            px <  SUPPORT_PANEL_X + SUPPORT_PANEL_W + 5 &&
            py >= SUPPORT_PANEL_Y - 5 &&
            py <  SUPPORT_PANEL_Y + SUPPORT_PANEL_H + 5) {
            int span = SUPPORT_PANEL_H + 10;
            int i = (py - (SUPPORT_PANEL_Y - 5)) * COIN_COUNT / span;
            s_coin = (i < COIN_COUNT) ? i : COIN_COUNT - 1;
            *outId = K_TAB_SWAP;
            return true;
        }
        /* From the same constants the button is drawn from. Written
         * out as 88 and 168 it was a second copy of the button's
         * position, and the block it belongs to has moved twice. */
        if (py >= SUPPORT_CLOSE_Y && py < SUPPORT_CLOSE_Y + SUPPORT_CLOSE_H &&
            px >= SUPPORT_CLOSE_X && px < SUPPORT_CLOSE_X + SUPPORT_CLOSE_W) {
            *outId = K_CANCEL;
            return true;
        }
        return false;
    }

    if (s_mode == KB_PICK && py >= PICK_Y &&
        py < PICK_Y + PICK_ROWS * PICK_ROW_H) {
        int row = s_pickTop + (py - PICK_Y) / PICK_ROW_H;
        if (row < PickRows()) {
            s_pickSel = row;
            *outId = K_OK;          /* a tap on a row is the choice */
            s_hitRow = 4; s_hitCol = 3;
            return true;
        }
        return false;
    }

    if (py >= BOT_Y && py < BOT_Y + BOT_H) {
        for (int i = 0; i < BOT_COUNT; i++) {
            /* A confirmation has no grid, so only the two answers are
             * live - the editing keys would do nothing and inviting a tap
             * on them is just a way to look broken. */
            if (s_mode != KB_TYPING && kBottom[i].id != K_OK &&
                kBottom[i].id != K_CANCEL) continue;

            if (px >= kBottom[i].x && px < kBottom[i].x + kBottom[i].w) {
                *outId = kBottom[i].id;
                s_hitRow = 4; s_hitCol = i;
                return true;
            }
        }
    }
    return false;
}

bool Keyboard_HandleTouch(int px, int py, bool pressed, bool released)
{
    if (s_mode == KB_IDLE) return false;
    (void)released;
    if (!pressed) return false;

    int id;
    if (!KeyAt(px, py, &id)) {
        s_pressedId = 0;
        s_pressLeft = 0;

        /* A tap above the panel closes it.
         *
         * The picker and the confirmation are boxes with the page behind
         * them, and tapping that page did nothing - which on a
         * touchscreen reads as being stuck, because tapping beside a
         * panel that is in the way is the obvious thing to try.
         *
         * Above it, specifically, and not "anywhere that is not a
         * control": a confirmation has to ignore a tap that lands in its
         * own dead space, or a miss inside the box answers a question
         * the user was still reading. Typing is excluded outright -
         * there is work in it to lose - and the full-screen pages have
         * no outside to tap.
         *
         * It cancels rather than accepts, so nothing is chosen or
         * deleted by a tap that missed. */
        if ((s_mode == KB_PICK || s_mode == KB_CONFIRM) && py < PICK_Y - 8) {
            s_cancelled = true;
            s_mode = s_return;
            return true;
        }
        return false;
    }

    /* The cursor follows the stylus, and stops being drawn.
     *
     * Moving it is what keeps the d-pad where the pen left off, so the
     * two agree the moment a button is pressed again. Hiding it is what
     * keeps a hard outline from being parked on the last key tapped for
     * the rest of the session - which is what a ring that follows the
     * stylus actually looks like, and reads as a keyboard stuck on one
     * letter. */
    if (s_hitRow >= 0) { s_curRow = s_hitRow; s_curCol = s_hitCol; }
    s_padFocus = false;

    s_pressedId = id;
    s_pressLeft = PRESS_FRAMES;
    return Activate(id);
}

bool Keyboard_HandleButtons(int down)
{
    if (s_mode == KB_IDLE) return false;

    /* Something was pressed, so the cursor is worth showing again. */
    if (down) s_padFocus = true;

    const int KEY_A_ = 1, KEY_B_ = 2, KEY_R_ = 256, KEY_L_ = 512;
    const int KEY_RIGHT_ = 16, KEY_LEFT_ = 32, KEY_UP_ = 64, KEY_DOWN_ = 128;

    bool changed = false;

    if (down & KEY_UP_)    { s_curRow = (s_curRow + 4) % 5; changed = true; }
    if (down & KEY_DOWN_)  { s_curRow = (s_curRow + 1) % 5; changed = true; }

    /* A confirmation draws two answers and not the three editing keys,
     * so those are where the cursor can go. Cycling through all five put
     * it on keys that are neither drawn nor live, and A on one of them
     * silently ran a backspace on an empty field - a cursor you cannot
     * see and a button that does nothing. */
    const int first = (s_mode == KB_CONFIRM && s_curRow == 4) ? BOT_COUNT - 2 : 0;
    int width = (s_curRow == 4) ? BOT_COUNT - first
              : (s_mode == KB_CONFIRM ? BOT_COUNT : (int)strlen(kRows[s_curRow]));
    if (width < 1) width = 1;
    if (s_curCol < first) { s_curCol = first; changed = true; }

    if (down & KEY_LEFT_)  { s_curCol = first + (s_curCol - first + width - 1) % width; changed = true; }
    if (down & KEY_RIGHT_) { s_curCol = first + (s_curCol - first + 1) % width; changed = true; }
    if (s_curCol >= first + width) { s_curCol = first + width - 1; changed = true; }

    /* A confirmation only has the bottom strip, so the cursor stays
     * there whatever up and down are pressed. */
    if (s_mode == KB_CONFIRM && s_curRow != 4) { s_curRow = 4; changed = true; }

    if (s_mode == KB_ENTRY) {
        static const int kIds[3] = { K_ENTRY_OPEN, K_ENTRY_HELP,
                                     K_ENTRY_SUPPORT };
        if (down & KEY_UP_)   { s_curRow = (s_curRow + 2) % 3; changed = true; }
        if (down & KEY_DOWN_) { s_curRow = (s_curRow + 1) % 3; changed = true; }
        if (down & KEY_A_)    changed |= Activate(kIds[s_curRow]);
        /* No B. There is nothing behind the start page to go back to, and
         * a B that silently opened the player would be a second, hidden
         * way to do what the big button on the screen already says. */
        return changed;
    }

    if (s_mode == KB_HELP) {
        if (down & (KEY_A_ | KEY_B_)) changed |= Activate(K_CANCEL);
        return changed;
    }


    if (s_mode == KB_SUPPORT) {
        /* Up and down, because the switch is a column - it was a strip
         * across the top when this was written and left/right was the
         * axis it ran along. Left and right still work, so a habit built
         * on the old layout is not punished.
         *
         * Both of them used to step forward. On a two-tab switch that is
         * invisible, since either direction reaches the other tab; with
         * five rows, pressing back walked you further away. */
        int step = 0;
        if (down & (KEY_DOWN_ | KEY_RIGHT_)) step = +1;
        if (down & (KEY_UP_   | KEY_LEFT_))  step = -1;
        if (step) {
            s_coin = (s_coin + step + COIN_COUNT) % COIN_COUNT;
            changed = true;
        }
        if (down & (KEY_A_ | KEY_B_)) changed |= Activate(K_CANCEL);
        return changed;
    }

    /* The picker uses up and down for the list instead. */
    if (s_mode == KB_PICK) {
        if (down & KEY_UP_)   { if (s_pickSel > 0) s_pickSel--; changed = true; }
        if (down & KEY_DOWN_) { if (s_pickSel < PickRows() - 1) s_pickSel++; changed = true; }
        if (s_pickSel < s_pickTop) s_pickTop = s_pickSel;
        if (s_pickSel >= s_pickTop + PICK_ROWS) s_pickTop = s_pickSel - PICK_ROWS + 1;
        s_curRow = 4;
        if (down & KEY_A_) changed |= Activate(K_OK);
        if (down & KEY_B_) changed |= Activate(K_CANCEL);
        return changed;
    }

    if (down & KEY_A_) {
        int id;
        if (s_curRow == 4) id = kBottom[s_curCol].id;
        else id = CasedChar(kRows[s_curRow][s_curCol]);
        changed |= Activate(id);
    }

    /* B is backspace while typing and "no" on a question - which had no
     * key at all, so the only way out of one was to find Cancel with the
     * d-pad or to tap it. Every other page here answers to B. */
    if (down & KEY_B_) {
        if (s_mode == KB_CONFIRM) changed |= Activate(K_CANCEL);
        else                      { Backspace(); changed = true; }
    }
    if (down & KEY_L_)  { s_lower = !s_lower; changed = true; }
    if (down & KEY_R_)  { Append(' '); changed = true; }

    return changed;
}

/* ---- drawing ------------------------------------------------------ */

/* Grouped by where a binding applies, because several keys mean
 * different things on different pages and a flat list of thirteen would
 * make that look like a contradiction. A NULL key marks a heading. */
static const struct { const char *key, *what; } kHelp[] = {
    { NULL, "ANYWHERE"           },
    { "L / R",        "Previous / next"    },
    { "L + R",        "Play / pause"       },
    { "X / Y",        "Next / previous"    },
    { "START",        "Change page"        },
    { "Lid shut",     "L / R still work"   },
    { NULL, "NOW PLAYING"        },
    { "Left / Right", "Seek"               },
    { "Up / Down",    "Volume"             },
    { "A or B",       "Play / pause"       },
    { "Y / X",        "Shuffle / repeat"   },   /* overrides the pair above, on this page only */
    { NULL, "FILES"              },
    { "A / B",        "Open / up a folder" },
};
#define HELP_ROWS ((int)(sizeof(kHelp) / sizeof(kHelp[0])))

/* Where each line sits, with the air before a heading folded in.
 *
 * Evenly spaced, the headings were just three of thirteen lines and the
 * grouping they exist to show had to be read rather than seen. A gap
 * ahead of each one costs two rows' worth of page across the whole table
 * and turns it into three blocks.
 *
 * A function rather than a stride because the spacing is no longer
 * uniform, and shared with the test that checks the table clears the
 * Back button - the static assert it replaces could only ever know about
 * a stride, so a heading added below would have slipped past it. */
static int HelpRowY(int i)
{
    int y = HELP_Y;
    for (int r = 1; r <= i; r++) {
        y += HELP_ROW_H;
        if (!kHelp[r].key) y += HELP_SECTION_GAP;
    }
    return y;
}

/* The ring the d-pad leaves on the key it is on.
 *
 * In the accent and hugging the key, not in the page's ink and standing
 * off it: a white box around a key is the loudest thing on the screen,
 * louder than the key it is marking and louder than the accent-filled
 * button beside it, and it was reading as an error rather than as a
 * cursor. Only drawn while the d-pad is what is being used. */
static void DrawFocus(int x, int y, int w, int h)
{
    if (!s_padFocus) return;
    Gfx_Frame(x - 1, y - 1, w + 2, h + 2, Skin_ColorAccent());
}

/* The same list-and-note the file browser draws beside a playlist, so a
 * row here and a row there are visibly the same kind of thing. Drawn
 * from its top rather than one pixel down, which is the only difference
 * between this and DrawListIcon in skin.c. */
static void DrawPickListIcon(int x, int y, uint16_t c)
{
    Gfx_Fill(x, y,     5, 2, c);
    Gfx_Fill(x, y + 3, 5, 2, c);
    Gfx_Fill(x, y + 6, 3, 2, c);

    Gfx_Fill(x + 9, y,     2, 6, c);     /* stem */
    Gfx_Fill(x + 6, y + 5, 4, 3, c);     /* head */
}

/* And a plus for the row that makes one, which is the only row on the
 * page that does not name something that already exists. */
static void DrawPickPlusIcon(int x, int y, uint16_t c)
{
    Gfx_Fill(x,     y + 3, 9, 3, c);
    Gfx_Fill(x + 3, y,     3, 9, c);
}

/* The name without the extension.
 *
 * ".m3u" is how the list is stored, not what it is called, and the
 * picker is the one place the user was asked to choose between
 * "Focus.m3u" and "Road trip.m3u". The file list already hides it. */
static void StripM3u(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s", name ? name : "");

    size_t len = strlen(out);
    if (len >= 5 && !strcasecmp(out + len - 5, ".m3u8")) out[len - 5] = '\0';
    else if (len >= 4 && !strcasecmp(out + len - 4, ".m3u")) out[len - 4] = '\0';
}

static void DrawKey(int x, int y, int w, int h, const char *label,
                    bool focused, bool accent, bool down)
{
    bool onAccent = down || accent;
    uint16_t face = onAccent ? Skin_ColorAccent() : Skin_ColorRaised();

    /* The label takes the ink for the ground it lands on, not the ink
     * for the page. A key filled with the accent - because it is the
     * major action, or because a finger is on it - drew its label in the
     * page's text colour anyway, so on a theme with a pale accent the
     * button you were pressing was the one you could not read, and the
     * feedback for pressing it was the label disappearing. */
    uint16_t ink = onAccent ? Skin_ColorTextOnSel() : Skin_ColorText();

    Gfx_Fill(x, y, w, h, face);

    /* A lighter hairline along the top edge. One line is the difference
     * between a key and a painted rectangle at this size - the same
     * device the transport buttons use, so the two read as the same
     * kind of control. */
    if (!down && !accent)
        Gfx_HLine(x, y, w, Skin_ColorHairline());

    Gfx_Text(x + (w - Gfx_TextWidth(label)) / 2, y + (h - GFX_GLYPH_H) / 2,
             ink, label);

    if (focused) DrawFocus(x, y, w, h);
}

void Keyboard_Draw(void)
{
    if (s_mode == KB_IDLE) return;

    Gfx_Fill(0, 0, 256, 192, Skin_ColorBg());
    Gfx_Fill(0, 0, 256, 3, Skin_ColorAccent());
    /* Only the modals that were opened with a prompt draw one. The three
     * standalone pages write their own headings, and the prompt left over
     * from whatever ran last would sit on top of them. */
    if (s_mode == KB_TYPING || s_mode == KB_CONFIRM || s_mode == KB_PICK)
        Gfx_Text(8, 10, Skin_ColorTextDim(), s_prompt);

    /* A rule under the prompt on the two pages whose body is not itself
     * a bordered thing. The typing page has the field for that, and a
     * rule above it would only be a second edge four pixels from the
     * first. */
    if (s_mode == KB_CONFIRM || s_mode == KB_PICK)
        Gfx_HLine(8, HEAD_RULE_Y, 240, Skin_ColorHairline());

    if (s_mode == KB_TYPING) {
        Gfx_Fill(FIELD_X, FIELD_Y, FIELD_W, FIELD_H, Skin_ColorSurface());
        Gfx_TextClipped(FIELD_X + 5, FIELD_Y + 5, FIELD_W - 16,
                        Skin_ColorText(), GFX_NO_SHADOW, s_text);

        /* A caret, so an empty field still reads as somewhere to type
         * rather than as a blank strip. */
        int cx = FIELD_X + 5 + Gfx_TextWidth(s_text);
        if (s_caretOn && cx < FIELD_X + FIELD_W - 4)
            Gfx_Fill(cx + 1, FIELD_Y + 4, 2, FIELD_H - 8, Skin_ColorAccent());

        char buf[2] = { 0, 0 };
        for (int r = 0; r < 4; r++) {
            int y = KB_Y + r * (KB_KEY_H + KB_GAP);
            int len = (int)strlen(kRows[r]);
            int x0 = KB_X + RowOffset(r);
            for (int c = 0; c < len; c++) {
                char ch = CasedChar(kRows[r][c]);
                buf[0] = ch;
                DrawKey(x0 + c * (KB_KEY_W + KB_GAP), y, KB_KEY_W, KB_KEY_H,
                        buf, s_curRow == r && s_curCol == c, false,
                        s_pressedId == (int)(unsigned char)ch);
            }
        }
    } else if (s_mode == KB_PICK) {
        /* One plate with the rows cut out of it, the way the support
         * page's switch is drawn: at this size a stack of separate
         * plates reads as loose buttons, and this is one list. */
        Gfx_Fill(PICK_X, PICK_Y, PICK_W, PICK_ROWS * PICK_ROW_H,
                 Skin_ColorSurface());

        for (int i = 0; i < PICK_ROWS; i++) {
            int row = s_pickTop + i;
            if (row >= PickRows()) break;

            int y = PICK_Y + i * PICK_ROW_H;
            bool sel = (row == s_pickSel);

            /* The label takes the ink for the ground it lands on. Drawn
             * in the page's ink either way, a theme with a pale accent
             * put white on white and the row you had chosen was the one
             * you could not read - the same fault the support page's
             * currency column had. */
            uint16_t ink = sel ? Skin_ColorTextOnSel() : Skin_ColorText();

            if (sel) {
                Gfx_Fill(PICK_X, y, PICK_W, PICK_ROW_H, Skin_ColorAccent());
            } else if (i > 0 && row - 1 != s_pickSel) {
                /* A hairline between neighbours, and none against the
                 * accent - a rule touching a filled row only muddies its
                 * edge. */
                Gfx_HLine(PICK_X + 8, y, PICK_W - 16, Skin_ColorHairline());
            }

            /* The extras come first and read differently from the names
             * below them - they are the only rows that do not name
             * something that already exists, so they get a plus where a
             * list gets its three bars. */
            const bool isNew = (row < s_extraCount);
            const char *label = isNew ? s_extras[row]
                                      : s_names[row - s_extraCount];

            int iy = y + (PICK_ROW_H - PICK_ICON_H) / 2;
            if (isNew) DrawPickPlusIcon(PICK_X + 8, iy, ink);
            else       DrawPickListIcon(PICK_X + 8, iy, ink);

            /* The extension is how the file is stored, not what the list
             * is called. Everywhere else shows the name; only here was
             * the user asked to pick between "Focus.m3u" and
             * "Road trip.m3u". */
            char shown[FB_NAME_LEN];
            StripM3u(shown, sizeof(shown), label);

            Gfx_TextClipped(PICK_X + 8 + PICK_ICON_W + 7,
                            y + (PICK_ROW_H - GFX_GLYPH_H) / 2,
                            PICK_W - (8 + PICK_ICON_W + 7) - 8,
                            ink, GFX_NO_SHADOW, shown);
        }

        /* The same rail the file list has, in the same place and the
         * same width - it is the same gesture on the same kind of list,
         * and two scrollbars of different sizes on one app is a detail
         * that only ever reads as an oversight. */
        if (PickRows() > PICK_ROWS) {
            const int trackY = PICK_Y, trackH = PICK_ROWS * PICK_ROW_H;
            const int innerH = trackH - 2;

            int thumbH = innerH * PICK_ROWS / PickRows();
            if (thumbH < 24) thumbH = 24;

            const int span = PickRows() - PICK_ROWS;
            int ty = trackY + 1 +
                     ((innerH - thumbH) * s_pickTop) / (span > 0 ? span : 1);

            Gfx_Fill(PICK_RAIL_X, trackY, PICK_RAIL_W, trackH,
                     Skin_ColorTrough());
            Gfx_Fill(PICK_RAIL_X + 1, ty, PICK_RAIL_W - 2, thumbH,
                     Skin_ColorAccent());
        }
    } else if (s_mode == KB_ENTRY) {
        Gfx_TextBig((256 - Gfx_TextBigWidth(3, 6, "DSXP")) / 2, 16, 3, 6,
                    Skin_ColorAccent(), "DSXP");

        const char *sub = "MP3 player for Nintendo DS";
        const char *by  = "by Farzin Abdi";
        Gfx_Text((256 - Gfx_TextWidth(sub)) / 2, 48, Skin_ColorText(), sub);
        Gfx_Text((256 - Gfx_TextWidth(by)) / 2, 62, Skin_ColorTextDim(), by);
        Gfx_HLine(48, 80, 160, Skin_ColorHairline());

        /* The major action is the only thing on the page at double text
         * size, and the only one filled in the accent colour. Two signals
         * for one idea, but the page has three buttons and a glance has to
         * land on the right one without reading all three. */
        Gfx_Fill(ENTRY_BTN_X, ENTRY_MAIN_Y, ENTRY_BTN_W, ENTRY_MAIN_H,
                 Skin_ColorAccent());
        /* Advance 7, not 4. Gfx_TextBig is monospaced and `advance` is
         * the cell width *before* scaling, so a cell has to be wider than
         * the widest glyph or the letters overlap - at scale 2 an advance
         * of 4 gives an eight-pixel cell to a twelve-pixel glyph. */
        Gfx_TextBig(ENTRY_BTN_X + (ENTRY_BTN_W
                                   - Gfx_TextBigWidth(2, 7, "Open App")) / 2,
                    ENTRY_MAIN_Y + (ENTRY_MAIN_H - 2 * GFX_GLYPH_H) / 2,
                    2, 7, Skin_ColorTextOnSel(), "Open App");
        if (s_curRow == 0)
            DrawFocus(ENTRY_BTN_X, ENTRY_MAIN_Y, ENTRY_BTN_W, ENTRY_MAIN_H);

        DrawKey(ENTRY_BTN_X, ENTRY_ALT_Y, ENTRY_BTN_W, ENTRY_ALT_H, "Help",
                s_curRow == 1, false, s_pressedId == K_ENTRY_HELP);
        DrawKey(ENTRY_BTN_X, ENTRY_ALT2_Y, ENTRY_BTN_W, ENTRY_ALT_H, "Support",
                s_curRow == 2, false, s_pressedId == K_ENTRY_SUPPORT);

    } else if (s_mode == KB_HELP) {

        Gfx_Text((256 - Gfx_TextWidth("Controls")) / 2, 6,
                 Skin_ColorText(), "Controls");
        Gfx_HLine(8, 18, 240, Skin_ColorHairline());

        for (int i = 0; i < HELP_ROWS; i++) {
            int y = HelpRowY(i);
            if (!kHelp[i].key) {
                /* A rule in the air above the heading, which is space
                 * that already exists - so the divider costs no rows and
                 * the three blocks read as blocks at a glance rather
                 * than after reading the headings. */
                if (i > 0) Gfx_HLine(8, y - 4, 240, Skin_ColorHairline());
                Gfx_Text(8, y, Skin_ColorAccent(), kHelp[i].what);
            } else {
                Gfx_Text(16, y, Skin_ColorTextDim(), kHelp[i].key);
                Gfx_Text(104, y, Skin_ColorText(), kHelp[i].what);
            }
        }

        /* Filled, the way Close is on the support page: it is the only
         * control here and the only thing to do, so it should look like
         * it rather than like a key that happens to be focused. */
        DrawKey(88, HELP_BACK_Y, 80, HELP_BACK_H, "Back", false, true,
                s_pressedId == K_CANCEL);

    } else if (s_mode == KB_SUPPORT) {
        /* The thank-you and its heart are centred as one group, the mark
         * measured in rather than guessed at, so the line stays centred
         * if the wording ever changes. */
        {
            static const char kThanks[] = "Thanks for your support";
            int tw = Gfx_TextBigPropWidth(SUPPORT_HEAD_SZ, kThanks);
            int th = 8 * SUPPORT_HEAD_SZ / 4;
            int gx = (256 - (tw + 6 + HEART_W)) / 2;
            Gfx_TextBigProp(gx, SUPPORT_HEAD_Y, SUPPORT_HEAD_SZ,
                            Skin_ColorText(), kThanks);
            Gfx_Icon16(gx + tw + 6, SUPPORT_HEAD_Y + (th - HEART_H) / 2,
                       HEART_H, kIconHeart, Skin_ColorAccent());
        }
        Gfx_HLine(8, SUPPORT_RULE_Y, 240, Skin_ColorHairline());

        /* The column is one plate with the rows cut out of it, rather
         * than four plates with gaps showing the page between them: at
         * this size the second reads as four loose buttons and the first
         * reads as a switch, which is what it is. */
        Gfx_Fill(SUPPORT_PANEL_X, SUPPORT_PANEL_Y,
                 SUPPORT_PANEL_W, SUPPORT_PANEL_H, Skin_ColorRaised());

        for (int i = 0; i < COIN_COUNT; i++) {
            int y = SUPPORT_ROW_Y(i);
            bool on = (i == s_coin);

            /* The selected row is filled with the accent, so its mark and
             * label take the ink meant for that ground rather than the
             * ink for the page behind it. Drawn in Skin_ColorText both
             * ways, a theme whose accent is pale put white on white and
             * the currency you had chosen was the one you could not
             * read. */
            uint16_t ink = on ? Skin_ColorTextOnSel() : Skin_ColorText();

            if (on) {
                Gfx_Fill(SUPPORT_PANEL_X, y, SUPPORT_PANEL_W, SUPPORT_ROW_H,
                         Skin_ColorAccent());

                /* A point on the row's right edge, aimed at the code it
                 * selects. It is the one thing that says the column and
                 * the code are the same object rather than two things
                 * that happen to share a screen. */
                for (int k = 0; k < SUPPORT_NIB_W; k++) {
                    int half = SUPPORT_NIB_W - k;
                    Gfx_VLine(SUPPORT_PANEL_X + SUPPORT_PANEL_W + k,
                              y + SUPPORT_ROW_H / 2 - half,
                              half * 2 + 1, Skin_ColorAccent());
                }
            } else if (i > 0) {
                /* A hairline between neighbours, and none against the
                 * accent - a rule touching a filled row only muddies its
                 * edge. */
                if (i - 1 != s_coin)
                    Gfx_HLine(SUPPORT_PANEL_X + 6, y - 1,
                              SUPPORT_PANEL_W - 12, Skin_ColorHairline());
            }

            /* Mark and label ride the row's centre line, each centred on
             * its own height rather than sharing a top edge - the mark is
             * eleven rows and the text eight, so a shared top leaves the
             * pair looking dropped. */
            Gfx_Icon(SUPPORT_PANEL_X + 8, y + (SUPPORT_ROW_H - COIN_ICON_H) / 2,
                     COIN_ICON_H, kCoins[i].icon, ink);
            Gfx_Text(SUPPORT_PANEL_X + 8 + COIN_ICON_W + 6,
                     y + (SUPPORT_ROW_H - 8) / 2, ink, kCoins[i].label);
        }

        /* Three pixels a module, on white with a quiet zone.
         *
         * The white is not decoration: a scanner needs the light modules
         * lighter than the dark ones, and drawing the code in the theme's
         * colours on the theme's background is how you get a QR that
         * looks right and reads to nothing. The border matters for the
         * same reason - a code butted against other pixels has no edge to
         * find. */
        /* Three pixels a module with a three-module border: 117 pixels
         * square, which is as large as fits above the address and the
         * button. Module size matters more to a scanner than quiet zone
         * does once the code is on white, so the trade goes that way. */
        const int scale = SUPPORT_QR_SCALE, quiet = SUPPORT_QR_QUIET;
        int side = SUPPORT_QR_SIDE;
        int ox = SUPPORT_QR_X, oy = SUPPORT_QR_Y;

        Gfx_Fill(ox, oy, side, side, GFX_RGB8(0xFF, 0xFF, 0xFF));

        const uint8_t *bits = kCoins[s_coin].bits;
        for (int r = 0; r < QR_SIZE; r++) {
            const uint8_t *row = bits + r * QR_STRIDE;
            for (int c = 0; c < QR_SIZE; c++) {
                if (!(row[c >> 3] & (1u << (c & 7)))) continue;
                Gfx_Fill(ox + (quiet + c) * scale, oy + (quiet + r) * scale,
                         scale, scale, GFX_RGB8(0, 0, 0));
            }
        }

        /* The address in full underneath, split across two lines - it is
         * the thing to fall back on when a camera will not cooperate, so
         * it has to be readable rather than merely present. */
        const char *t = kCoins[s_coin].text;
        int len = (int)strlen(t);
        int half = (len + 1) / 2;
        char a[48], b[48];
        snprintf(a, sizeof(a), "%.*s", half, t);
        snprintf(b, sizeof(b), "%s", t + half);

        /* On the block's axis, under the code they belong to. */
        int ty = SUPPORT_ADDR_Y;
        Gfx_Text(SUPPORT_AXIS - Gfx_TextWidth(a) / 2, ty, Skin_ColorText(), a);
        Gfx_Text(SUPPORT_AXIS - Gfx_TextWidth(b) / 2, ty + 9,
                 Skin_ColorText(), b);
    } else {
        /* The thing being asked about, on a plate of its own. Set as a
         * loose line under the question it was the same weight as the
         * question, and which of the two was the playlist name took
         * reading rather than looking. */
        /* Centred in what the heading and the answers leave, rather
         * than pushed against the rule with eighty empty pixels under
         * it - the page is four lines of content on a whole screen, and
         * stacked at the top it read as a page that had failed to
         * finish drawing. */
        enum { PLATE_H = 22, WARN_GAP = 10 };
        const int blockH = PLATE_H + (s_destructive ? WARN_GAP + GFX_GLYPH_H : 0);
        const int top = HEAD_RULE_Y + 4;
        int py = top + (BOT_Y - 4 - top - blockH) / 2;

        Gfx_Fill(8, py, 240, PLATE_H, Skin_ColorSurface());
        Gfx_VLine(8, py, PLATE_H, Skin_ColorAccent());
        Gfx_VLine(9, py, PLATE_H, Skin_ColorAccent());

        char shown[FB_NAME_LEN];
        StripM3u(shown, sizeof(shown), s_subject);
        Gfx_TextClipped(18, py + (PLATE_H - GFX_GLYPH_H) / 2, 222,
                        Skin_ColorText(), GFX_NO_SHADOW, shown);

        /* Only where it is true. It was on every confirmation, including
         * the two that add tracks to a list - a warning that cries wolf
         * on the safe ones is one nobody reads on the destructive
         * ones. */
        if (s_destructive)
            Gfx_Text(8, py + PLATE_H + WARN_GAP, Skin_ColorTextDim(),
                     "This cannot be undone.");
    }

    if (s_mode == KB_SUPPORT) {
        DrawKey(SUPPORT_CLOSE_X, SUPPORT_CLOSE_Y, SUPPORT_CLOSE_W,
                SUPPORT_CLOSE_H, "Close", true, true,
                s_pressedId == K_CANCEL);
        return;
    }

    /* The start page and the help page draw their own buttons and are not
     * text entry, so the shared OK/Cancel strip has no business on them -
     * it was landing on top of Support and offering two answers to a page
     * that asks no question. */
    if (s_mode == KB_ENTRY || s_mode == KB_HELP)
        return;

    for (int i = 0; i < BOT_COUNT; i++) {
        if (s_mode != KB_TYPING && kBottom[i].id != K_OK &&
            kBottom[i].id != K_CANCEL) continue;

        const char *label = kBottom[i].label;
        if (kBottom[i].id == K_SHIFT) label = s_lower ? "ABC" : "abc";
        if (s_mode == KB_CONFIRM && kBottom[i].id == K_OK) label = s_verb;
        if (s_mode == KB_PICK    && kBottom[i].id == K_OK) label = "Choose";

        /* An empty name has nothing to accept - Activate already refuses
         * it - so the button says so instead of looking live and doing
         * nothing, which is the one thing a button must never do. */
        const bool dead = (s_mode == KB_TYPING &&
                           kBottom[i].id == K_OK && s_text[0] == '\0');

        /* The picker's focus is the highlighted row, not a key down
         * here: two cursors on one page point at two different answers,
         * and only one of them is what A does. */
        const bool focused = (s_mode != KB_PICK) &&
                             s_curRow == 4 && s_curCol == i;

        DrawKey(kBottom[i].x, BOT_Y, kBottom[i].w, BOT_H, label,
                focused, kBottom[i].id == K_OK && !dead,
                s_pressedId == kBottom[i].id);
    }
}
