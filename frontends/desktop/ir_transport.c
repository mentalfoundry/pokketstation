/* The calls that each transport shares. They forward to the table of the transport that
   ir_transport_init_pipe or ir_transport_init_tcp installed. See ir_transport.h. */
#include "ir_transport.h"

int ir_transport_host(ir_transport_t *t, const char *address) {
    return t->vt->host(t, address);
}

int ir_transport_connect(ir_transport_t *t, const char *address) {
    return t->vt->connect(t, address);
}

void ir_transport_poll(ir_transport_t *t) {
    t->vt->poll(t);
}

size_t ir_transport_send(ir_transport_t *t, const void *data, size_t size) {
    return t->vt->send(t, data, size);
}

size_t ir_transport_recv(ir_transport_t *t, void *data, size_t size) {
    return t->vt->recv(t, data, size);
}

void ir_transport_close(ir_transport_t *t) {
    t->vt->close(t);
}
