#include "socket.h"

#include "object.h"
#include "util.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define CONNECTION_BUFFER_SIZE (16 * 1024 * 1024)

void varlink_socket_init(VarlinkSocket *socket) {
        socket->fd = -1;

        socket->in = malloc(CONNECTION_BUFFER_SIZE);
        socket->in_start = 0;
        socket->in_end = 0;

        socket->out = malloc(CONNECTION_BUFFER_SIZE);
        socket->out_start = 0;
        socket->out_end = 0;

        socket->hup = false;
}

void varlink_socket_deinit(VarlinkSocket *socket) {
        if (socket->fd >= 0)
                close(socket->fd);

        free(socket->in);
        free(socket->out);
}

static void move_rest(uint8_t **bufferp, unsigned long *startp, unsigned long *endp) {
        uint8_t *buffer;
        unsigned long start, end, rest;

        buffer = *bufferp;
        start = *startp;
        end = *endp;

        rest = end - start;
        if (rest > 0)
                *bufferp = memmove(buffer, buffer + start, rest);

        *startp = 0;
        *endp = rest;
}

long varlink_socket_dispatch(VarlinkSocket *socket, int events) {
        if (events & EPOLLIN) {
                long n;

                move_rest(&socket->in, &socket->in_start, &socket->in_end);

                n = read(socket->fd,
                         socket->in + socket->in_end,
                         CONNECTION_BUFFER_SIZE - socket->in_end);

                switch (n) {
                        case -1:
                                if (errno != EAGAIN)
                                        return -VARLINK_ERROR_RECEIVING_MESSAGE;
                                break;
                        case 0:
                                socket->hup = true;
                                break;
                        default:
                                socket->in_end += n;
                                break;
                }
        }

        if (events & EPOLLOUT) {
                long n;

                n = write(socket->fd,
                          socket->out + socket->out_start,
                          socket->out_end - socket->out_start);

                switch (n) {
                        case -1:
                                if (errno != EAGAIN)
                                        return -VARLINK_ERROR_SENDING_MESSAGE;
                                break;
                        default:
                                socket->out_start += n;
                                break;
                }

                move_rest(&socket->out, &socket->out_start, &socket->out_end);
        }

        return 0;
}

int varlink_socket_get_events(VarlinkSocket *socket) {
        int events = 0;

        if (socket->in_end < CONNECTION_BUFFER_SIZE && !socket->hup)
                events |= EPOLLIN;

        if (socket->out_start < socket->out_end)
                events |= EPOLLOUT;

        return events;
}

long varlink_socket_read(VarlinkSocket *socket, VarlinkObject **messagep) {
        uint8_t *base;
        uint8_t *nul;
        long r;

        base = socket->in + socket->in_start;
        nul = memchr(base, 0, socket->in_end - socket->in_start);
        if (!nul) {
                *messagep = NULL;

                if (socket->in_start == socket->in_end && socket->hup)
                        return -VARLINK_ERROR_CONNECTION_CLOSED;

                return 0;
        }

        r = varlink_object_new_from_json(messagep, (const char *)base);
        if (r < 0)
                return r;

        socket->in_start += nul - base + 1; /* 1 for the \0 */

        return 0;
}

long varlink_socket_write(VarlinkSocket *socket, VarlinkObject *message) {
        _cleanup_(freep) char *json = NULL;
        long length;

        length = varlink_object_to_pretty_json(message, &json, -1, NULL, NULL, NULL, NULL);
        if (length < 0)
                return length;

        if (length >= CONNECTION_BUFFER_SIZE - 1)
                return VARLINK_ERROR_INVALID_MESSAGE;

        if (socket->out_end + length + 1 >= CONNECTION_BUFFER_SIZE)
                return VARLINK_ERROR_SENDING_MESSAGE;

        memcpy(socket->out + socket->out_end, json, length + 1);
        socket->out_end += length + 1;

        return 0;
}