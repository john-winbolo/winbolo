#ifndef WINBOLO_INPUT_GATE_H
#define WINBOLO_INPUT_GATE_H

#include <stdbool.h>

/* Live overlay state classified for the in-game input gate. Blocking
   surfaces suspend the polled in-game readers; transient notifications
   (alliance request, vote widgets) must never suspend them. */
typedef struct InputGateState {
    bool textInputActive;              /* a text field wants keyboard input   */
    bool blockingModalOpen;            /* a focus-stealing panel is open       */
    bool menuOpen;                     /* a blocking popup/menu is on the stack*/
    bool allianceNotificationVisible;  /* transient — never suspends           */
    bool voteVisible;                  /* transient — never suspends           */
    bool appHasFocus;                  /* OS window has input focus            */
} InputGateState;

/* Returns true when the in-game readers should bail to TNONE.
   allianceNotificationVisible and voteVisible are deliberately NOT
   suspend triggers; they are carried so the decision is explicit and
   so a future regression that re-adds them is caught by the unit test. */
static inline bool gameInputSuspended(const InputGateState *s) {
    if (!s->appHasFocus)      return true;
    if (s->textInputActive)   return true;
    if (s->blockingModalOpen) return true;
    if (s->menuOpen)          return true;
    return false;
}

#endif /* WINBOLO_INPUT_GATE_H */
