#ifndef COOP_HOST_H
#define COOP_HOST_H

#include <stdbool.h>

/* In-process World Host. Listens on 127.0.0.1:27770 and speaks the same
 * HELLO / READY / AES-GCM framing the game client already uses. The game
 * itself still connects as a client, so gameplay does not grow a second path. */

void CoopHost_Start(void);
bool CoopHost_Listening(void);

#endif
