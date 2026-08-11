#include "sdkconfig.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include "esp_netif.h"
#include "esp_log.h"

// Returns full response
// Pointer to data and data length
uint8_t *http_request(char *path, uint8_t **payload, unsigned *length) {
    struct sockaddr_in sock_desc;
    int socket_client;

    socket_client = socket(AF_INET,SOCK_STREAM,0);
    if (socket_client < 0) return NULL;

    inet_pton(AF_INET, CONFIG_EPD_WEB_SERVER, &sock_desc.sin_addr);
    sock_desc.sin_family = AF_INET;
    sock_desc.sin_port = htons(CONFIG_EPD_WEB_SERVER_PORT);

    if (connect(socket_client,(struct sockaddr *)&sock_desc,sizeof(sock_desc)) < 0) {
	close(socket_client);
	return NULL;
    }

    char req[256];

    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: epd/1.0 esp32\r\n\r\n", path, CONFIG_EPD_WEB_SERVER, CONFIG_EPD_WEB_SERVER_PORT);

    if (write(socket_client, req, strlen(req)) < 0) {
	close(socket_client);
	return NULL;
    }

    int count = 0;

    uint8_t *data, *ptr;

#define MAX_BYTES (50*1024)

    data = malloc(MAX_BYTES);
    ptr = data;

    while(count < MAX_BYTES) {
	int ret;

	if ((ret = read(socket_client, ptr, MAX_BYTES-count)) < 0) {
	    break;
	}
	count += ret;
	ptr += ret;
    }

    close(socket_client);

    *payload = NULL;
    *length = 0;

    // Skip HTTP headers
    ptr = data;
    while(ptr < &data[count]) {
	if (strncmp((char *)ptr, "\r\n\r\n",4) == 0) {
            *payload = ptr + 4;
            *length = (&data[count]) - (*payload);
	    break;
	}
	ptr++;
    }

    printf("Got total %d bytes for '%s'. Payload length: %d\n", count, path, *length);

    return data; // Full response
}
