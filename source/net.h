#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

#define NET_DEFAULT_UA "VLC/3.0.20 LibVLC/3.0.20"

int net_init(void);
void net_shutdown(void);

/* Downloads a whole resource (http/https URL or local /dev_... path) into memory.
   Returns malloc'd NUL-terminated buffer, or NULL with `err` filled in. */
char *net_get(const char *url, const char *ua, size_t *out_len, char *err, size_t errlen);

typedef struct stream stream_t;

/* Opens a live byte stream. HLS (.m3u8) playlists are followed transparently and
   their MPEG-TS segments are concatenated into one continuous stream.
   max_height selects the HLS variant (e.g. 576). */
stream_t *stream_open(const char *url, const char *ua, const char *referer, int max_height);
int stream_read(stream_t *s, uint8_t *buf, int size);  /* >0 bytes, 0 EOF, <0 error/abort */
void stream_abort(stream_t *s);                        /* non-blocking */
void stream_close(stream_t *s);                        /* aborts + joins + frees */
int stream_fill_percent(stream_t *s);
int stream_failed(stream_t *s, char *msg, size_t msglen);
void stream_info(stream_t *s, char *out, size_t outlen);  /* e.g. "HLS 1280x720" */
uint64_t stream_bytes(stream_t *s);

#endif
