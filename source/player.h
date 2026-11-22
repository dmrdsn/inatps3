#ifndef PLAYER_H
#define PLAYER_H

#include <stddef.h>
#include "gfx.h"

typedef enum {
    PS_IDLE = 0,
    PS_CONNECTING,
    PS_BUFFERING,
    PS_PLAYING,
    PS_ENDED,
    PS_ERROR
} player_state;

int player_init(void);          /* starts the audio output thread; call after gfx_init */
void player_shutdown(void);

void player_play(const char *url, const char *ua, const char *referer, int max_height);
void player_stop(void);

player_state player_get_state(char *msg, size_t msglen);
void player_info(char *out, size_t outlen);

/* Called once per displayed frame by the render loop. Returns 1 and fills `out`
   when there is a picture to draw. */
int player_video_frame(video_tex *out);
void player_frame_done(void);   /* call after gfx_frame_end() */

#endif
