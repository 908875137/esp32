#ifndef _HTTP_H_
#define _HTTP_H_

// Very simplistic HTTP request function
// Query server and return full response, payload and length
// Must be freed after use
uint8_t *http_request(char *path, uint8_t **payload, unsigned *length);

#endif
