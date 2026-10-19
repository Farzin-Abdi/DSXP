/* The naming modal.
 *
 * What matters here is not that keys append characters - that is visible
 * by reading it - but the rules that stop a name becoming something the
 * card cannot hold or the user cannot find again. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "nds.h"
#include "filebrowser.h"
#include "visualizer.h"

/* skin.c reaches into the browser for the list metrics, and into the
 * visualizer for the meter picker's style names; this test exercises
 * neither, so the handful of calls they need are enough. */
const char *Visualizer_StyleName(int i)
{
    static const char *const kNames[] = { "Bars", "Blocks", "Mirror", "Wave",
                                          "Wide", "Dots", "Comb", "Hang" };
    return (i >= 0 && i < 8) ? kNames[i] : "?";
}

const char *Top_ChoiceName(int choice)
{
    if (TOP_IS_METER(choice)) return Visualizer_StyleName(choice);
    return "Card";
}

static int g_visRows = FB_MAX_VISIBLE_ROWS;
int  FileBrowser_VisibleRows(void) { return g_visRows; }
void FileBrowser_SetVisibleRows(int r) { g_visRows = r; }
int  FileBrowser_MarkedCount(const FileBrowser *fb) { (void)fb; return 0; }

#include "gfx.c"
#include "skin.c"

/* The mixer is not built here, and the skin asks it for the volume so
 * the gramophone's slider can show something true. */
int Audio_Volume(void) { return 96; }

#include "keyboard.c"

static int g_fail = 0;
static void Check(const char *what, bool ok, const char *detail)
{
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) g_fail++;
}

/* Taps the key whose label matches, by walking the same tables the
 * drawing uses - so a key that moved is a test that moves with it. */
/* Taps by the key's own label, not by the character it will produce -
 * the case is the keyboard's decision and is what the tests are checking,
 * so a helper that searched for the cased character would only ever find
 * keys it already agreed with. */
static void TapChar(char want)
{
    if (want >= 'a' && want <= 'z') want -= 32;

    for (int r = 0; r < 4; r++) {
        int len = (int)strlen(kRows[r]);
        int x0 = KB_X + RowOffset(r);
        for (int c = 0; c < len; c++) {
            char ch = kRows[r][c];
            if (ch != want) continue;
            Keyboard_HandleTouch(x0 + c * (KB_KEY_W + KB_GAP) + 2,
                                 KB_Y + r * (KB_KEY_H + KB_GAP) + 2, true, false);
            return;
        }
    }
}

static void TapBottom(int id)
{
    for (int i = 0; i < BOT_COUNT; i++)
        if (kBottom[i].id == id)
            Keyboard_HandleTouch(kBottom[i].x + 2, BOT_Y + 2, true, false);
}

int main(void)
{
    printf("naming modal\n");
    Gfx_InitFont();

    char buf[160];

    /* A name starts with a capital and carries on in lower case without
     * anyone asking for it - the thing every phone does and the reason
     * nobody thinks about shift when naming something. */
    Keyboard_OpenText("Name", "", 40);
    TapChar('M'); TapChar('i'); TapChar('x'); TapBottom(K_SPACE); TapChar('1');
    snprintf(buf, sizeof(buf), "typed '%s' with no shift presses", s_text);
    Check("the first letter capitalises itself",
          strcmp(s_text, "Mix 1") == 0, buf);

    /* Deleting back to nothing is back to the start of a name. */
    for (int i = 0; i < 5; i++) TapBottom(K_BACK);
    TapChar('A');
    snprintf(buf, sizeof(buf), "typed '%s' after clearing", s_text);
    Check("emptying the field restores the capital",
          strcmp(s_text, "A") == 0, buf);

    /* Pressing shift by hand is a lock: a shift that undoes itself when
     * you meant to hold it is worse than one that does nothing. */
    Keyboard_OpenText("Name", "", 40);
    TapChar('D');                    /* auto capital, then lower */
    TapBottom(K_SHIFT);              /* deliberate: back to upper */
    TapChar('V'); TapChar('D');
    snprintf(buf, sizeof(buf), "typed '%s'", s_text);
    Check("a deliberate shift stays until pressed again",
          strcmp(s_text, "DVD") == 0, buf);

    Keyboard_OpenText("Name", "", 40);
    TapChar('M'); TapBottom(K_SHIFT); TapChar('i'); TapChar('x');
    TapBottom(K_SPACE);
    TapBottom(K_SHIFT); TapChar('1');
    snprintf(buf, sizeof(buf), "typed '%s'", s_text);
    Check("a shift held through a word capitalises it",
          strcmp(s_text, "MIX 1") == 0, buf);

    TapBottom(K_BACK);
    snprintf(buf, sizeof(buf), "now '%s'", s_text);
    Check("backspace removes one character", strcmp(s_text, "MIX ") == 0, buf);

    /* Characters a FAT path cannot hold are refused, not substituted: a
     * name silently altered on the way to the card is a file the user
     * cannot find again. */
    size_t before = strlen(s_text);
    Keyboard_HandleTouch(0, 0, true, false);      /* nothing there */
    Check("no key on the grid produces a path separator",
          strchr(s_text, '/') == NULL && strchr(s_text, '\\') == NULL,
          "the grid offers letters, digits and space only");
    Check("a tap on empty space does nothing", strlen(s_text) == before, NULL);

    /* An empty name must not be accepted: refusing beats inventing one. */
    Keyboard_OpenText("Name", "", 40);
    TapBottom(K_OK);
    Check("OK on an empty field does not close it",
          Keyboard_IsOpen() && Keyboard_Mode() == KB_TYPING, NULL);

    TapChar('A');
    TapBottom(K_OK);
    const char *got = NULL;
    bool accepted = Keyboard_TakeAccepted(&got);
    snprintf(buf, sizeof(buf), "accepted '%s'", accepted && got ? got : "(none)");
    Check("OK with text closes and returns it",
          accepted && got && strcmp(got, "A") == 0 && !Keyboard_IsOpen(), buf);

    /* Cancel must report cancelled, not accepted - saving something the
     * user backed out of is worse than not saving. */
    Keyboard_OpenText("Name", "Draft", 40);
    TapBottom(K_CANCEL);
    Check("cancel closes without accepting",
          !Keyboard_IsOpen() && !Keyboard_TakeAccepted(NULL) &&
          Keyboard_TakeCancelled(), NULL);

    /* The field cannot outgrow its buffer. */
    Keyboard_OpenText("Name", "", 8);
    for (int i = 0; i < 40; i++) TapChar('X');
    snprintf(buf, sizeof(buf), "%d characters with a limit of 8", (int)strlen(s_text));
    Check("typing stops at the limit", strlen(s_text) == 7, buf);

    /* Confirm mode offers two answers and nothing else. */
    Keyboard_OpenConfirm("Delete?", "Mix.m3u", "Delete", true);
    TapBottom(K_SPACE);
    Check("a confirmation ignores the editing keys",
          Keyboard_Mode() == KB_CONFIRM, NULL);
    Check("and the yes button says what it will do",
          !strcmp(s_verb, "Delete"), s_verb);

    /* The cursor has nowhere to be but the two answers. It used to cycle
     * through all five keys of the strip, three of which a confirmation
     * does not draw - so A could land on a key that was not on the
     * screen and silently run a backspace on an empty field. */
    {
        int off = 0;
        for (int i = 0; i < 12; i++) {
            Keyboard_HandleButtons(16);      /* right */
            if (s_curCol < BOT_COUNT - 2) off++;
        }
        for (int i = 0; i < 12; i++) {
            Keyboard_HandleButtons(32);      /* left */
            if (s_curCol < BOT_COUNT - 2) off++;
        }
        snprintf(buf, sizeof(buf), "%d presses landed off the answers", off);
        Check("left and right stay on the two answers", off == 0, buf);
    }

    /* And B answers no. It was a backspace here - the key every other
     * page in this modal uses to get out did nothing at all on the one
     * page that asks a question. */
    Keyboard_HandleButtons(2);               /* B */
    Check("B cancels a confirmation",
          !Keyboard_IsOpen() && Keyboard_TakeCancelled(), NULL);

    Keyboard_OpenConfirm("Delete?", "Mix.m3u", "Delete", true);
    TapBottom(K_OK);
    Check("and the answer key closes it",
          !Keyboard_IsOpen() && Keyboard_TakeAccepted(NULL), NULL);

    /* The cursor follows the stylus, and stops being drawn while the
     * stylus is what is being used. Both halves matter: moving it keeps
     * the d-pad where the pen left off, and hiding it is what stops a
     * ring being parked on the last key tapped for the rest of a
     * touch-only session - indistinguishable from a keyboard stuck on
     * that key. */
    Keyboard_OpenText("Name", "", 40);
    int startRow = s_curRow, startCol = s_curCol;
    TapChar('M');
    snprintf(buf, sizeof(buf), "moved from (%d,%d) to (%d,%d)",
             startRow, startCol, s_curRow, s_curCol);
    Check("a tap moves the cursor to that key",
          s_curRow != startRow || s_curCol != startCol, buf);
    Check("and a tap does not leave a ring behind", !s_padFocus, NULL);

    Keyboard_HandleButtons(32);             /* left */
    Check("a button press brings the ring back", s_padFocus, NULL);
    TapChar('N');
    Check("and the next tap takes it away again", !s_padFocus, NULL);

    /* The caret has to blink, and typing has to leave it visible - a
     * caret that happens to be dark when you look is a field that looks
     * dead. */
    s_caretOn = false;
    TapChar('X');
    Check("typing leaves the caret showing", s_caretOn, NULL);

    int flips = 0;
    for (int i = 0; i < 200; i++) if (Keyboard_Tick()) flips++;
    snprintf(buf, sizeof(buf), "%d flips over 200 frames", flips);
    Check("the caret blinks while idle", flips >= 4 && flips <= 20, buf);

    /* The picker: row zero is "new", the rest are the names given. */
    static const char *const lists[] = { "A.m3u", "B.m3u" };
    static const char *const news[]  = { "New here", "New in /Playlists" };
    Keyboard_OpenPick("Add to", news, 2, lists, 2);
    Check("a picker starts on the first new row", Keyboard_PickedRow() == 0, NULL);

    /* The strip cursor has to agree with what A does. Left where the
     * previous page had it, it sat on Cancel while A chose the
     * highlighted row - one page showing two answers and giving a
     * third. */
    snprintf(buf, sizeof(buf), "cursor on column %d, Choose is %d",
             s_curCol, BOT_COUNT - 2);
    Check("and with its cursor on the button A presses",
          s_curRow == 4 && kBottom[s_curCol].id == K_OK, buf);

    for (int i = 0; i < 3; i++) Keyboard_HandleButtons(128);   /* down x3 */
    snprintf(buf, sizeof(buf), "row %d of 4", Keyboard_PickedRow());
    Check("down walks the extras and then the names",
          Keyboard_PickedRow() == 3, buf);

    for (int i = 0; i < 5; i++) Keyboard_HandleButtons(128);   /* past the end */
    snprintf(buf, sizeof(buf), "row %d", Keyboard_PickedRow());
    Check("and stops at the last row", Keyboard_PickedRow() == 3, buf);

    Keyboard_HandleButtons(1);              /* A - choose */
    Check("choosing closes with that row",
          !Keyboard_IsOpen() && Keyboard_TakeAccepted(NULL) &&
          Keyboard_PickedRow() == 3, NULL);

    /* The grid the pickers are walked on.
     *
     * Themes, colours and fonts all step through one function now; they
     * used to have three copies of the arithmetic, two of them in
     * main.c working from a column count written out again beside the
     * one in the skin. Checked here at the sizes that actually ship, and
     * at one that does not - a grid whose last row is short, which every
     * copy got wrong and which nothing would have caught until somebody
     * added a face or a theme. */
    {
        /* Every cell reachable from every other, both ways, on each of
         * the three real grids. */
        static const struct { const char *what; int count, cols; } kGrids[] = {
            { "themes",  SKIN_THEMES,      6 },
            { "colours", SKIN_ACCENTS,     8 },
            { "fonts",   GFX_FONT_STYLES,  4 },
        };

        for (unsigned g = 0; g < sizeof(kGrids) / sizeof(kGrids[0]); g++) {
            const int n = kGrids[g].count, c = kGrids[g].cols;
            const int rows = (n + c - 1) / c;

            /* Right and left run along a row, up and down along a
             * column, so reaching every cell takes both - walk each row
             * from its start. */
            unsigned long seen = 0;
            int bad = 0, at = 0;
            for (int r = 0; r < rows; r++) {
                for (int i = 0; i < c; i++) {
                    if (at < 0 || at >= n) { bad++; break; }
                    seen |= 1UL << at;
                    at = GridStep(at, n, c, +1, 0);
                }
                at = GridStep(at, n, c, 0, +1);
            }

            /* And both axes have to undo themselves everywhere. */
            for (int i = 0; i < n; i++) {
                if (GridStep(GridStep(i, n, c, +1, 0), n, c, -1, 0) != i) bad++;
                if (GridStep(GridStep(i, n, c, 0, +1), n, c, 0, -1) != i) bad++;
            }

            const unsigned long all = (n >= 64) ? ~0UL : ((1UL << n) - 1);
            snprintf(buf, sizeof(buf), "%s: %d cells %d wide, %d bad, %d reached",
                     kGrids[g].what, n, c, bad, __builtin_popcountl(seen));
            Check("every cell of a picker grid is reachable",
                  bad == 0 && seen == all, buf);
        }

        /* A grid that does not divide evenly: seven cells three wide, so
         * the last row holds one. Every step has to stay on the grid,
         * sideways along the short row must not walk into the two cells
         * that are not there, and down into a column that row lacks has
         * to land on the cell it does have. */
        int bad = 0;
        for (int i = 0; i < 7; i++) {
            if (GridStep(i, 7, 3, 0, +1) >= 7) bad++;
            if (GridStep(i, 7, 3, 0, -1) >= 7) bad++;
            if (GridStep(i, 7, 3, +1, 0) >= 7) bad++;
            if (GridStep(i, 7, 3, -1, 0) >= 7) bad++;
        }
        int onShort  = GridStep(6, 7, 3, +1, 0);   /* the lone last cell */
        int intoShort = GridStep(4, 7, 3, 0, +1);  /* middle column, into it */
        snprintf(buf, sizeof(buf), "%d off-grid, right from 6 is %d, down from 4 is %d",
                 bad, onShort, intoShort);
        Check("a short last row stays on the grid",
              bad == 0 && onShort == 6 && intoShort == 6, buf);
    }

    /* The controls table clears the Back button.
     *
     * This was a _Static_assert with the row count written into it - as
     * twelve, against a table of thirteen - so it went on passing while
     * the last binding was drawn underneath the button. Two things are
     * asserted here that it could not: the count comes from the table,
     * and the position comes from the same function the drawing uses, so
     * the air before each heading is counted rather than assumed away.
     */
    {
        int last = HelpRowY(HELP_ROWS - 1) + GFX_GLYPH_H;
        snprintf(buf, sizeof(buf), "%d rows end at %d, button at %d",
                 HELP_ROWS, last, HELP_BACK_Y);
        Check("the controls table clears the Back button",
              last <= HELP_BACK_Y - 4, buf);

        /* And the headings are the only lines with air above them - a
         * gap that landed anywhere else would read as a group boundary
         * where there is none. */
        int misplaced = 0;
        for (int i = 1; i < HELP_ROWS; i++) {
            int step = HelpRowY(i) - HelpRowY(i - 1);
            bool heading = (kHelp[i].key == NULL);
            if ((step > HELP_ROW_H) != heading) misplaced++;
        }
        snprintf(buf, sizeof(buf), "%d lines spaced wrongly", misplaced);
        Check("only the headings get air above them", misplaced == 0, buf);

        /* No key runs into the column of descriptions beside it. The two
         * columns are set at fixed x, so a longer binding than any there
         * now - "Left / Right" is the widest - would overlap the text it
         * is meant to label, on a page nothing else checks. */
        int over = 0, widest = 0;
        const char *worst = "";
        for (int i = 0; i < HELP_ROWS; i++) {
            if (!kHelp[i].key) continue;
            int wpx = Gfx_TextWidth(kHelp[i].key);
            if (wpx > widest) { widest = wpx; worst = kHelp[i].key; }
            if (16 + wpx >= 104) over++;
        }
        snprintf(buf, sizeof(buf), "widest \"%s\" ends at %d, column at 104",
                 worst, 16 + widest);
        Check("no binding runs into its description", over == 0, buf);
    }

    /* The support column under the d-pad.
     *
     * Both directions used to step forward, which a two-tab switch hides
     * completely - either key reaches the other tab - and five rows do
     * not. Walked in each direction and checked against where it should
     * be, including the wrap at both ends. */
    {
        const int KDOWN = 128, KUP = 64;
        Keyboard_OpenSupport();
        int ok = 1;
        for (int i = 1; i <= COIN_COUNT; i++) {
            Keyboard_HandleButtons(KDOWN);
            if (s_coin != i % COIN_COUNT) ok = 0;
        }
        snprintf(buf, sizeof(buf), "down %d times ends on %d",
                 COIN_COUNT, s_coin);
        Check("down walks the column and wraps", ok && s_coin == 0, buf);

        ok = 1;
        for (int i = 1; i <= COIN_COUNT; i++) {
            Keyboard_HandleButtons(KUP);
            if (s_coin != (COIN_COUNT - i % COIN_COUNT) % COIN_COUNT) ok = 0;
        }
        snprintf(buf, sizeof(buf), "up %d times ends on %d",
                 COIN_COUNT, s_coin);
        Check("up walks it the other way", ok && s_coin == 0, buf);
    }

    /* The support screen's currency switch.
     *
     * Tapped at the exact centre of each row as drawn, because the bug
     * this guards against was the hit test and the drawing disagreeing
     * about where the switch was: the tabs were painted at one row and
     * matched eleven lower, so only the bottom third of a tab did
     * anything. Deriving the tap from the drawing constants catches that;
     * a test with its own hardcoded coordinates drifts along with the
     * bug - which this one then did, in the other axis, when it kept a
     * written-out column through the move to four tabs and again through
     * the move from a strip across the top to a column down the left. */
    for (int want = 0; want < COIN_COUNT; want++) {
        Keyboard_OpenSupport();
        int cx = SUPPORT_PANEL_X + SUPPORT_PANEL_W / 2;
        int cy = SUPPORT_ROW_Y(want) + SUPPORT_ROW_H / 2;
        Keyboard_HandleTouch(cx, cy, true, false);
        snprintf(buf, sizeof(buf), "tap (%d,%d) -> coin %d", cx, cy, s_coin);
        Check("a tap on a currency row selects it", s_coin == want, buf);
    }

    /* Every column of the panel is live, out past the row's painted edges
     * on both sides - a thumb lands wide of a 58-pixel row often enough
     * that the margin is the point. */
    Keyboard_OpenSupport();
    int live = 0;
    const int probeCoin = COIN_COUNT - 1;
    const int probeY = SUPPORT_ROW_Y(probeCoin) + SUPPORT_ROW_H / 2;
    const int x0 = SUPPORT_PANEL_X - 5;
    const int x1 = SUPPORT_PANEL_X + SUPPORT_PANEL_W + 5;
    for (int x = x0; x < x1; x++) {
        s_coin = 0;
        Keyboard_HandleTouch(x, probeY, true, false);
        if (s_coin == probeCoin) live++;
    }
    snprintf(buf, sizeof(buf), "%d of %d columns", live, x1 - x0);
    Check("the whole panel width is a live target", live == x1 - x0, buf);

    /* A tap in the gap between two rows has to land on one of them.
     * Which one does not matter - that it is neither is the failure. */
    s_coin = -1;
    int gutterY = SUPPORT_ROW_Y(0) + SUPPORT_ROW_H + SUPPORT_ROW_GAP / 2;
    Keyboard_HandleTouch(SUPPORT_PANEL_X + SUPPORT_PANEL_W / 2, gutterY,
                         true, false);
    snprintf(buf, sizeof(buf), "tap y=%d -> coin %d", gutterY, s_coin);
    Check("and the gutter between the rows picks a side",
          s_coin == 0 || s_coin == 1, buf);

    /* The column and the code must not overlap. The code is drawn once
     * and the rows are repainted under the finger, so an overlap would
     * eat into a QR that a camera then cannot read - and the selected
     * row's point sticks out past the column, so the clearance is
     * measured from the point rather than from the plate. */
    {
        const int panelRight =
            SUPPORT_PANEL_X + SUPPORT_PANEL_W + SUPPORT_NIB_W;
        snprintf(buf, sizeof(buf), "column ends %d, code starts %d",
                 panelRight, SUPPORT_QR_X);
        Check("the code starts clear of the column",
              SUPPORT_QR_X > panelRight, buf);

        /* The column spans the page between the rule and the button.
         *
         * It used to be centred against the code, which was right while
         * it was the shorter of the two and became a squeeze on the rows
         * when it grew to five and took full names. What has to hold now
         * is that it clears the rule above and the button below - the
         * height it takes between them is the point, not a match to
         * anything else. */
        snprintf(buf, sizeof(buf), "rule %d, column %d..%d, button %d",
                 SUPPORT_RULE_Y, SUPPORT_PANEL_Y,
                 SUPPORT_PANEL_Y + SUPPORT_PANEL_H, SUPPORT_CLOSE_Y);
        Check("the column spans the page without touching either end",
              SUPPORT_PANEL_Y > SUPPORT_RULE_Y &&
              SUPPORT_PANEL_Y + SUPPORT_PANEL_H < SUPPORT_CLOSE_Y, buf);

        /* And every label fits the width it is given, which is the whole
         * reason the column got wider. A name that overruns is drawn
         * past the plate and into the gap, where nothing erases it. */
        int widest = 0;
        const char *worst = "";
        for (int i = 0; i < COIN_COUNT; i++) {
            int wpx = Gfx_TextWidth(kCoins[i].label);
            if (wpx > widest) { widest = wpx; worst = kCoins[i].label; }
        }
        int used = 8 + COIN_ICON_W + 6 + widest;
        snprintf(buf, sizeof(buf), "\"%s\" needs %d of %d",
                 worst, used, SUPPORT_PANEL_W);
        Check("the longest name fits the column", used <= SUPPORT_PANEL_W, buf);

        /* The code has to clear the button underneath it as well as the
         * column beside it, with the two address lines in between. */
        snprintf(buf, sizeof(buf), "code ends %d, address %d..%d, button %d",
                 SUPPORT_QR_Y + SUPPORT_QR_SIDE, SUPPORT_ADDR_Y,
                 SUPPORT_ADDR_Y + SUPPORT_ADDR_H, SUPPORT_CLOSE_Y);
        Check("the code and address clear the button",
              SUPPORT_QR_Y + SUPPORT_QR_SIDE <= SUPPORT_ADDR_Y &&
              SUPPORT_ADDR_Y + SUPPORT_ADDR_H <= SUPPORT_CLOSE_Y, buf);

        /* And the button clears the bottom edge. It sat two pixels off
         * it, which is a page that has run out rather than ended - and
         * two pixels is inside the tolerance of a resistive panel, so
         * the last row of it was barely there to press. */
        snprintf(buf, sizeof(buf), "button ends %d of 192",
                 SUPPORT_CLOSE_Y + SUPPORT_CLOSE_H);
        Check("the button clears the bottom edge",
              SUPPORT_CLOSE_Y + SUPPORT_CLOSE_H <= 192 - 6, buf);
    }

    /* The heading, the code and the address all centred on the screen,
     * measured off the pixels rather than off the constants.
     *
     * They have been on two axes before: the code centred in the space
     * the column left and the address centred across the full width,
     * thirty-four pixels apart. Each is centred on something, which is
     * why reading either line of the drawing code finds nothing wrong -
     * only holding them against each other shows it. So this renders the
     * page and measures where the ink actually lands. The bands are
     * bounded by constants that exist for other reasons, so there is no
     * second copy of the layout here to drift. */
    {
        static uint16_t fb[256 * 192];
        Skin_Init(fb);
        Keyboard_OpenSupport();
        Skin_BeginFrame();
        Keyboard_Draw();

        const int colX0 =
            SUPPORT_PANEL_X + SUPPORT_PANEL_W + SUPPORT_NIB_W + 1;
        const uint16_t ground = fb[(SUPPORT_CLOSE_Y - 4) * 256 + 250];

        /* The heading spans the whole width above both column and block,
         * so it is centred on the screen; the code, its address and the
         * button share the block's axis. Each band carries the axis it
         * is meant to be on, so the test states the intent rather than
         * merely repeating whatever the drawing does. */
        struct { const char *name; int y0, y1, x0, axis; int lo, hi; } band[] = {
            { "heading", SUPPORT_HEAD_Y - 2, SUPPORT_RULE_Y - 1, 0,
              256 / 2, 999, -1 },
            { "code",    SUPPORT_QR_Y, SUPPORT_QR_Y + SUPPORT_QR_SIDE,
              colX0, SUPPORT_AXIS, 999, -1 },
            { "address", SUPPORT_QR_Y + SUPPORT_QR_SIDE, SUPPORT_CLOSE_Y - 2,
              colX0, SUPPORT_AXIS, 999, -1 },
            { "button",  SUPPORT_CLOSE_Y, SUPPORT_CLOSE_Y + SUPPORT_CLOSE_H,
              colX0, SUPPORT_AXIS, 999, -1 },
        };
        const int bands = (int)(sizeof(band) / sizeof(band[0]));

        for (int i = 0; i < bands; i++)
            for (int y = band[i].y0; y < band[i].y1; y++)
                for (int x = band[i].x0; x < 256; x++) {
                    if (fb[y * 256 + x] == ground) continue;
                    if (x < band[i].lo) band[i].lo = x;
                    if (x > band[i].hi) band[i].hi = x;
                }

        for (int i = 0; i < bands; i++) {
            /* Doubled throughout, so an axis that falls between two
             * pixels is not a rounding error the test has to forgive. */
            int off = band[i].lo + band[i].hi - (2 * band[i].axis - 1);
            snprintf(buf, sizeof(buf), "%s %d..%d, off its axis by %d",
                     band[i].name, band[i].lo, band[i].hi, off / 2);
            Check("drawn on the axis it belongs to",
                  band[i].hi > band[i].lo && off <= 2 && off >= -2, buf);
        }

        /* The block leans right of the screen's centre, past the column,
         * but not so far that it reads as having slid off the page. */
        Check("the block leans right without leaving the screen",
              SUPPORT_AXIS > 256 / 2 && SUPPORT_QR_X + SUPPORT_QR_SIDE < 256,
              NULL);

        /* And the heading fits, which a line set half again as large as
         * the body text is the first thing on this page not to. */
        snprintf(buf, sizeof(buf), "heading spans %d..%d",
                 band[0].lo, band[0].hi);
        Check("the heading fits on the screen",
              band[0].lo >= 0 && band[0].hi < 256, buf);
    }

    /* The start page and where its three buttons go. */
    Keyboard_OpenEntry();
    Keyboard_HandleTouch(128, ENTRY_ALT_Y + 8, true, false);
    Check("Help opens the controls page", Keyboard_Mode() == KB_HELP, NULL);

    Keyboard_HandleTouch(128, HELP_BACK_Y + 8, true, false);
    Check("and Back returns to the start page",
          Keyboard_Mode() == KB_ENTRY, NULL);

    Keyboard_HandleTouch(128, ENTRY_ALT2_Y + 8, true, false);
    Check("Support opens from the start page",
          Keyboard_Mode() == KB_SUPPORT, NULL);

    Keyboard_HandleTouch(SUPPORT_CLOSE_X + SUPPORT_CLOSE_W / 2,
                         SUPPORT_CLOSE_Y + SUPPORT_CLOSE_H / 2, true, false);
    Check("and Close returns to the start page it came from",
          Keyboard_Mode() == KB_ENTRY, NULL);

    Keyboard_HandleTouch(128, ENTRY_MAIN_Y + 16, true, false);
    Check("Open App leaves the modal entirely", !Keyboard_IsOpen(), NULL);
    (void)Keyboard_TakeCancelled();

    /* Opened from the titlebar instead, Close has no hub to go back to. */
    Keyboard_OpenSupport();
    Keyboard_HandleTouch(SUPPORT_CLOSE_X + SUPPORT_CLOSE_W / 2,
                         SUPPORT_CLOSE_Y + SUPPORT_CLOSE_H / 2, true, false);
    Check("but from the titlebar it closes outright", !Keyboard_IsOpen(), NULL);
    (void)Keyboard_TakeCancelled();

    printf("\n%s\n", g_fail ? "FAILURES" : "ALL PASSED (0 failures)");
    return g_fail ? 1 : 0;
}
