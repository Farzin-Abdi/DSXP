#ifndef DSXP_KEYBOARD_H
#define DSXP_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>

/* A modal text field and key grid for the bottom screen.
 *
 * The DS has no keyboard and eight buttons, so anything that needs a name
 * typed has to draw one. This owns the whole bottom screen while it is
 * open - tabs and transport included - because a modal that leaves live
 * controls around its edges invites taps that go somewhere unexpected
 * while the user is mid-word.
 *
 * Also serves as the confirmation dialog: a question with two answers is
 * the same modal with the grid left out, and sharing it means there is
 * one thing that owns the screen rather than two that might both think
 * they do. */

typedef enum {
    KB_IDLE = 0,     /* nothing open */
    KB_TYPING,       /* text entry */
    KB_CONFIRM,      /* a yes/no question */
    KB_PICK,         /* choose one of a list */
    KB_SUPPORT,      /* the support screen */
    KB_ENTRY,        /* the start page, shown once before the player */
    KB_HELP,         /* the controls reference */
} KeyboardMode;

/* `prompt` is the line above the field. `initial` seeds the field and may
 * be NULL. `maxLen` counts the terminator. */
void Keyboard_OpenText(const char *prompt, const char *initial, size_t maxLen);
/* `verb` is what the yes button says - "Delete", "Add", "Remove". It
 * used to always say Delete, because the only confirmation that existed
 * when it was written was a deletion; by the time there were four, "Add
 * 3 to this playlist?" was being answered with a button marked Delete.
 *
 * `destructive` adds the line that says it cannot be undone, which for
 * the same reason used to be on every one of them - including the two
 * that only add. */
void Keyboard_OpenConfirm(const char *prompt, const char *subject,
                          const char *verb, bool destructive);

/* Choose one of `count` names, preceded by `extraCount` fixed rows -
 * the "make a new one somewhere" options, which are not names of things
 * that exist. Borrowed pointers: both arrays must outlive the modal. */
void Keyboard_OpenPick(const char *prompt,
                       const char *const *extras, int extraCount,
                       const char *const *names, int count);

/* The row chosen, counting the extras first. Only meaningful on the frame
 * Keyboard_TakeAccepted reports a KB_PICK. */
int Keyboard_PickedRow(void);

/* The support screen: a name and a payment address as a QR code, with
 * the two currencies as a choice. Shares the modal because it wants the
 * same thing the others do - the whole screen, and nothing underneath it
 * taking taps meant for it. */
void Keyboard_OpenSupport(void);

/* The start page: what the app is, who wrote it, and the three places to
 * go from here. Opened once before the player appears.
 *
 * Help and Support open from it and return to it, so the start page is a
 * hub rather than a splash that has to be dismissed before anything can
 * be read. The titlebar heart opens Support directly from anywhere and
 * closes straight back to the player - same screen, different way home. */
void Keyboard_OpenEntry(void);
void Keyboard_OpenHelp(void);


/* A live readout of what the hardware is actually reporting. Repaints
 * continuously, unlike every other page here. */

/* The register scanner: reads every ARM7 I/O register and reports the
 * ones that consistently differ between headphones-out and headphones-in.
 * Separate from the readout page because it needs the user to declare
 * which state the console is in, repeatedly. */

KeyboardMode Keyboard_Mode(void);

/* Shuts the modal without accepting or cancelling it.
 *
 * For the caller that is moving somewhere else entirely and is not
 * answering the question the modal asked - START cycling out of the
 * About tab, which is the support page rather than a view of its own. */
void Keyboard_Close(void);
static inline bool Keyboard_IsOpen(void) { return Keyboard_Mode() != KB_IDLE; }

/* Feed input while open. Returns true if the modal wants a repaint. */
bool Keyboard_HandleTouch(int px, int py, bool pressed, bool released);
bool Keyboard_HandleButtons(int down);

/* Set for exactly one frame after the modal closes. */
bool Keyboard_TakeAccepted(const char **outText);
bool Keyboard_TakeCancelled(void);

/* Call once a frame while open. Returns true when the caret blink needs
 * the modal repainted. */
bool Keyboard_Tick(void);

void Keyboard_Draw(void);

#endif /* DSXP_KEYBOARD_H */
