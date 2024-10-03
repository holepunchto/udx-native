#include <node_api.h>
#include <stdlib.h>
#include <string.h>
#include <udx.h>
#include <uv.h>

#include "macros.h"

#define UDX_NAPI_INTERACTIVE     0
#define UDX_NAPI_NON_INTERACTIVE 1
#define UDX_NAPI_FRAMED          2

typedef struct {
  udx_t udx;

  char *read_buf;
  size_t read_buf_free;
} udx_napi_t;

typedef struct {
  udx_socket_t socket;
  udx_napi_t *udx;

  napi_env env;
  napi_ref ctx;
  napi_ref on_send;
  napi_ref on_message;
  napi_ref on_close;
  napi_ref realloc_message;
} udx_napi_socket_t;

typedef struct {
  udx_stream_t stream;
  udx_napi_t *udx;

  int mode;

  char *read_buf;
  char *read_buf_head;
  size_t read_buf_free;

  ssize_t frame_len;

  napi_env env;
  napi_ref ctx;
  napi_ref on_data;
  napi_ref on_end;
  napi_ref on_drain;
  napi_ref on_ack;
  napi_ref on_send;
  napi_ref on_message;
  napi_ref on_close;
  napi_ref on_firewall;
  napi_ref on_remote_changed;
  napi_ref realloc_data;
  napi_ref realloc_message;
} udx_napi_stream_t;

typedef struct {
  udx_lookup_t handle;

  char *host;

  napi_env env;
  napi_ref ctx;
  napi_ref on_lookup;
} udx_napi_lookup_t;

typedef struct {
  udx_interface_event_t handle;

  napi_env env;
  napi_ref ctx;
  napi_ref on_event;
  napi_ref on_close;
} udx_napi_interface_event_t;

inline static void
parse_address (struct sockaddr *name, char *ip, size_t size, int *port, int *family) {
  if (name->sa_family == AF_INET) {
    *port = ntohs(((struct sockaddr_in *) name)->sin_port);
    *family = 4;
    uv_ip4_name((struct sockaddr_in *) name, ip, size);
  } else if (name->sa_family == AF_INET6) {
    *port = ntohs(((struct sockaddr_in6 *) name)->sin6_port);
    *family = 6;
    uv_ip6_name((struct sockaddr_in6 *) name, ip, size);
  }
}

static void
on_udx_send (udx_socket_send_t *req, int status) {
  udx_napi_socket_t *n = (udx_napi_socket_t *) req->socket;

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_send, &callback);

  napi_value argv[2];
  napi_create_int32(env, (uintptr_t) req->data, &(argv[0]));
  napi_create_int32(env, status, &(argv[1]));

  if (napi_make_callback(env, NULL, ctx, callback, 2, argv, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_message (udx_socket_t *self, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  udx_napi_socket_t *n = (udx_napi_socket_t *) self;

  int port = 0;
  char ip[INET6_ADDRSTRLEN];
  int family = 0;
  parse_address((struct sockaddr *) from, ip, INET6_ADDRSTRLEN, &port, &family);

  if (buf->len > n->udx->read_buf_free) return;

  memcpy(n->udx->read_buf, buf->base, buf->len);

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_message, &callback);

  napi_value argv[4];
  napi_create_uint32(env, read_len, &(argv[0]));
  napi_create_uint32(env, port, &(argv[1]));
  napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, family, &(argv[3]));

  napi_value res;

  if (napi_make_callback(env, NULL, ctx, callback, 4, argv, &res) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
    {
      napi_env env = n->env;

      napi_handle_scope scope;
      napi_open_handle_scope(env, &scope);

      napi_value ctx;
      napi_get_reference_value(env, n->ctx, &ctx);

      napi_value callback;
      napi_get_reference_value(env, n->realloc_message, &callback);

      if (napi_make_callback(env, NULL, ctx, callback, 0, NULL, &res) == napi_pending_exception) {
        napi_value fatal_exception;
        napi_get_and_clear_last_exception(env, &fatal_exception);
        napi_fatal_exception(env, fatal_exception);
      }

      napi_get_buffer_info(env, res, (void **) &(n->udx->read_buf), &(n->udx->read_buf_free));

      napi_close_handle_scope(env, scope);
    }
  } else {
    napi_get_buffer_info(env, res, (void **) &(n->udx->read_buf), &(n->udx->read_buf_free));
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_close (udx_socket_t *self) {
  udx_napi_socket_t *n = (udx_napi_socket_t *) self;

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_close, &callback);

  if (napi_make_callback(env, NULL, ctx, callback, 0, NULL, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);

  napi_delete_reference(env, n->on_send);
  napi_delete_reference(env, n->on_message);
  napi_delete_reference(env, n->on_close);
  napi_delete_reference(env, n->realloc_message);
  napi_delete_reference(env, n->ctx);
}

static void
on_udx_stream_end (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  size_t read = n->read_buf_head - n->read_buf;

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_end, &callback);

  napi_value argv[1];
  napi_create_uint32(env, read, &(argv[0]));

  if (napi_make_callback(env, NULL, ctx, callback, 1, argv, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_stream_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len == UV_EOF) return on_udx_stream_end(stream);

  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  // ignore the message if it doesn't fit in the read buffer
  if (buf->len > n->read_buf_free) return;

  if (n->mode == UDX_NAPI_FRAMED && n->frame_len == -1) {
    if (buf->len < 3) {
      n->mode = UDX_NAPI_INTERACTIVE;
    } else {
      uint8_t *b = (uint8_t *) buf->base;
      n->frame_len = 3 + (b[0] | (b[1] << 8) | (b[2] << 16));
    }
  }

  memcpy(n->read_buf_head, buf->base, buf->len);

  n->read_buf_head += buf->len;
  n->read_buf_free -= buf->len;

  if (n->mode == UDX_NAPI_NON_INTERACTIVE && n->read_buf_free >= 2 * stream->mtu) {
    return;
  }

  ssize_t read = n->read_buf_head - n->read_buf;

  if (n->mode == UDX_NAPI_FRAMED) {
    if (n->frame_len < read) {
      n->mode = UDX_NAPI_INTERACTIVE;
    } else if (n->frame_len == read) {
      n->frame_len = -1;
    } else if (n->read_buf_free < 2 * stream->mtu) {
      n->frame_len -= read;
    } else {
      return; // wait for more data
    }
  }

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_data, &callback);

  napi_value argv[1];
  napi_create_uint32(env, read, &(argv[0]));

  napi_value res;

  if (napi_make_callback(env, NULL, ctx, callback, 1, argv, &res) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
    {
      napi_env env = n->env;

      napi_handle_scope scope;
      napi_open_handle_scope(env, &scope);

      napi_value ctx;
      napi_get_reference_value(env, n->ctx, &ctx);

      napi_value callback;
      napi_get_reference_value(env, n->realloc_data, &callback);

      if (napi_make_callback(env, NULL, ctx, callback, 0, NULL, &res) == napi_pending_exception) {
        napi_value fatal_exception;
        napi_get_and_clear_last_exception(env, &fatal_exception);
        napi_fatal_exception(env, fatal_exception);
      }

      napi_get_buffer_info(env, res, (void **) &(n->read_buf), &(n->read_buf_free));
      n->read_buf_head = n->read_buf;

      napi_close_handle_scope(env, scope);
    }
  } else {
    napi_get_buffer_info(env, res, (void **) &(n->read_buf), &(n->read_buf_free));
    n->read_buf_head = n->read_buf;
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_stream_drain (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_drain, &callback);

  if (napi_make_callback(env, NULL, ctx, callback, 0, NULL, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_stream_ack (udx_stream_write_t *req, int status, int unordered) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) req->stream;

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_ack, &callback);

  napi_value argv[1];
  napi_create_uint32(env, (uintptr_t) req->data, &(argv[0]));

  if (napi_make_callback(env, NULL, ctx, callback, 1, argv, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_stream_send (udx_stream_send_t *req, int status) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) req->stream;

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_send, &callback);

  napi_value argv[2];
  napi_create_int32(env, (uintptr_t) req->data, &(argv[0]));
  napi_create_int32(env, status, &(argv[1]));

  if (napi_make_callback(env, NULL, ctx, callback, 2, argv, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_stream_recv (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  if (buf->len > n->udx->read_buf_free) return;

  memcpy(n->udx->read_buf, buf->base, buf->len);

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_message, &callback);

  napi_value argv[1];
  napi_create_uint32(env, read_len, &(argv[0]));

  napi_value res;

  if (napi_make_callback(env, NULL, ctx, callback, 1, argv, &res) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
    {
      napi_env env = n->env;

      napi_handle_scope scope;
      napi_open_handle_scope(env, &scope);

      napi_value ctx;
      napi_get_reference_value(env, n->ctx, &ctx);

      napi_value callback;
      napi_get_reference_value(env, n->realloc_message, &callback);

      if (napi_make_callback(env, NULL, ctx, callback, 0, NULL, &res) == napi_pending_exception) {
        napi_value fatal_exception;
        napi_get_and_clear_last_exception(env, &fatal_exception);
        napi_fatal_exception(env, fatal_exception);
      }

      napi_get_buffer_info(env, res, (void **) &(n->udx->read_buf), &(n->udx->read_buf_free));

      napi_close_handle_scope(env, scope);
    }
  } else {
    napi_get_buffer_info(env, res, (void **) &(n->udx->read_buf), &(n->udx->read_buf_free));
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_stream_finalize (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  napi_delete_reference(n->env, n->on_data);
  napi_delete_reference(n->env, n->on_end);
  napi_delete_reference(n->env, n->on_drain);
  napi_delete_reference(n->env, n->on_ack);
  napi_delete_reference(n->env, n->on_send);
  napi_delete_reference(n->env, n->on_message);
  napi_delete_reference(n->env, n->on_close);
  napi_delete_reference(n->env, n->on_firewall);
  napi_delete_reference(n->env, n->on_remote_changed);
  napi_delete_reference(n->env, n->realloc_data);
  napi_delete_reference(n->env, n->realloc_message);
  napi_delete_reference(n->env, n->ctx);
}

static void
on_udx_stream_close (udx_stream_t *stream, int status) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_close, &callback);

  napi_value argv[1];

  if (status >= 0) {
    napi_get_null(env, &(argv[0]));
  } else {
    napi_value code;
    napi_value msg;
    napi_create_string_utf8(env, uv_err_name(status), NAPI_AUTO_LENGTH, &code);
    napi_create_string_utf8(env, uv_strerror(status), NAPI_AUTO_LENGTH, &msg);
    napi_create_error(env, code, msg, &(argv[0]));
  }

  if (napi_make_callback(env, NULL, ctx, callback, 1, argv, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);
}

static int
on_udx_stream_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  udx_napi_socket_t *s = (udx_napi_socket_t *) socket;

  uint32_t fw = 1; // assume error means firewall it, whilst reporting the uncaught

  int port = 0;
  char ip[INET6_ADDRSTRLEN];
  int family = 0;
  parse_address((struct sockaddr *) from, ip, INET6_ADDRSTRLEN, &port, &family);

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_firewall, &callback);

  napi_value res;
  napi_value argv[4];

  napi_get_reference_value(env, s->ctx, &(argv[0]));
  napi_create_uint32(env, port, &(argv[1]));
  napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, family, &(argv[3]));

  if (napi_make_callback(env, NULL, ctx, callback, 4, argv, &res) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  } else {
    napi_get_value_uint32(env, res, &fw);
  }

  napi_close_handle_scope(env, scope);

  return fw;
}

static void
on_udx_stream_remote_changed (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  napi_env env = n->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_remote_changed, &callback);

  if (napi_make_callback(env, NULL, ctx, callback, 0, NULL, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_lookup (udx_lookup_t *lookup, int status, const struct sockaddr *addr, int addr_len) {
  udx_napi_lookup_t *n = (udx_napi_lookup_t *) lookup;

  napi_env env = n->env;

  char ip[INET6_ADDRSTRLEN] = "";
  int family = 0;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, n->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, n->on_lookup, &callback);

  if (status >= 0) {
    if (addr->sa_family == AF_INET) {
      uv_ip4_name((struct sockaddr_in *) addr, ip, addr_len);
      family = 4;
    } else if (addr->sa_family == AF_INET6) {
      uv_ip6_name((struct sockaddr_in6 *) addr, ip, addr_len);
      family = 6;
    }

    napi_value argv[3];
    napi_get_null(env, &(argv[0]));
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &(argv[1]));
    napi_create_uint32(env, family, &(argv[2]));

    if (napi_make_callback(env, NULL, ctx, callback, 3, argv, NULL) == napi_pending_exception) {
      napi_value fatal_exception;
      napi_get_and_clear_last_exception(env, &fatal_exception);
      napi_fatal_exception(env, fatal_exception);
    }
  } else {
    napi_value argv[1];
    napi_value code;
    napi_value msg;
    napi_create_string_utf8(env, uv_err_name(status), NAPI_AUTO_LENGTH, &code);
    napi_create_string_utf8(env, uv_strerror(status), NAPI_AUTO_LENGTH, &msg);
    napi_create_error(env, code, msg, &(argv[0]));

    if (napi_make_callback(env, NULL, ctx, callback, 1, argv, NULL) == napi_pending_exception) {
      napi_value fatal_exception;
      napi_get_and_clear_last_exception(env, &fatal_exception);
      napi_fatal_exception(env, fatal_exception);
    }
  }

  free(n->host);

  napi_close_handle_scope(env, scope);

  napi_delete_reference(n->env, n->on_lookup);
  napi_delete_reference(n->env, n->ctx);
}

static void
on_udx_interface_event (udx_interface_event_t *handle, int status) {
  udx_napi_interface_event_t *e = (udx_napi_interface_event_t *) handle;

  napi_env env = e->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, e->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, e->on_event, &callback);

  if (napi_make_callback(env, NULL, ctx, callback, 0, NULL, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);
}

static void
on_udx_interface_event_close (udx_interface_event_t *handle) {
  udx_napi_interface_event_t *e = (udx_napi_interface_event_t *) handle;

  napi_env env = e->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, e->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, e->on_close, &callback);

  if (napi_make_callback(env, NULL, ctx, callback, 0, NULL, NULL) == napi_pending_exception) {
    napi_value fatal_exception;
    napi_get_and_clear_last_exception(env, &fatal_exception);
    napi_fatal_exception(env, fatal_exception);
  }

  napi_close_handle_scope(env, scope);

  napi_delete_reference(env, e->on_event);
  napi_delete_reference(env, e->on_close);
  napi_delete_reference(env, e->ctx);
}

napi_value
udx_napi_init (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_napi_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  char *read_buf;
  size_t read_buf_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &read_buf, &read_buf_len))

  uv_loop_t *loop;
  napi_get_uv_event_loop(env, &loop);

  udx_init(loop, &(self->udx));

  self->read_buf = read_buf;
  self->read_buf_free = read_buf_len;

  return NULL;
}

napi_value
udx_napi_socket_init (napi_env env, napi_callback_info info) {
  napi_value argv[7];
  size_t argc = 7;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_napi_t *udx;
  size_t udx_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &udx, &udx_len))

  udx_napi_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &self, &self_len))

  udx_socket_t *socket = (udx_socket_t *) self;

  self->udx = udx;
  self->env = env;
  napi_create_reference(env, argv[2], 1, &(self->ctx));
  napi_create_reference(env, argv[3], 1, &(self->on_send));
  napi_create_reference(env, argv[4], 1, &(self->on_message));
  napi_create_reference(env, argv[5], 1, &(self->on_close));
  napi_create_reference(env, argv[6], 1, &(self->realloc_message));

  int err = udx_socket_init((udx_t *) udx, socket);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_socket_bind (napi_env env, napi_callback_info info) {
  napi_value argv[5];
  size_t argc = 5;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  uint32_t port;
  if (napi_get_value_uint32(env, argv[1], &port) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  char ip[INET6_ADDRSTRLEN];
  size_t ip_len;
  if (napi_get_value_string_utf8(env, argv[2], (char *) &ip, INET6_ADDRSTRLEN, &ip_len) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected string");
    return NULL;
  }

  uint32_t family;
  if (napi_get_value_uint32(env, argv[3], &family) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  uint32_t flags;
  if (napi_get_value_uint32(env, argv[4], &flags) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  int err;

  struct sockaddr_storage addr;
  int addr_len;

  if (family == 4) {
    addr_len = sizeof(struct sockaddr_in);
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    addr_len = sizeof(struct sockaddr_in6);
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  err = udx_socket_bind(self, (struct sockaddr *) &addr, flags);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  // TODO: move the bottom stuff into another function, start, so error handling is easier

  struct sockaddr_storage name;

  // wont error in practice
  err = udx_socket_getsockname(self, (struct sockaddr *) &name, &addr_len);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  int local_port;

  if (family == 4) {
    local_port = ntohs(((struct sockaddr_in *) &name)->sin_port);
  } else {
    local_port = ntohs(((struct sockaddr_in6 *) &name)->sin6_port);
  }

  // wont error in practice
  err = udx_socket_recv_start(self, on_udx_message);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, local_port, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_socket_set_ttl (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  uint32_t ttl;
  if (napi_get_value_uint32(env, argv[1], &ttl) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  int err = udx_socket_set_ttl(self, ttl);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_socket_get_recv_buffer_size (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  int size = 0;

  int err = udx_socket_get_recv_buffer_size(self, &size);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, size, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_socket_set_recv_buffer_size (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  int32_t size;
  if (napi_get_value_int32(env, argv[1], &size) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected number");
    return NULL;
  }

  int err = udx_socket_set_recv_buffer_size(self, size);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_socket_get_send_buffer_size (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  int size = 0;

  int err = udx_socket_get_send_buffer_size(self, &size);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, size, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_socket_set_send_buffer_size (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  int32_t size;
  if (napi_get_value_int32(env, argv[1], &size) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected number");
    return NULL;
  }

  int err = udx_socket_set_send_buffer_size(self, size);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, size, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_socket_send_ttl (napi_env env, napi_callback_info info) {
  napi_value argv[8];
  size_t argc = 8;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  udx_socket_send_t *req;
  size_t req_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &req, &req_len))

  uint32_t rid;
  if (napi_get_value_uint32(env, argv[2], &rid) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  char *buf;
  size_t buf_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[3], (void **) &buf, &buf_len))

  uint32_t port;
  if (napi_get_value_uint32(env, argv[4], &port) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  char ip[INET6_ADDRSTRLEN];
  size_t ip_len;
  if (napi_get_value_string_utf8(env, argv[5], (char *) &ip, INET6_ADDRSTRLEN, &ip_len) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected string");
    return NULL;
  }

  uint32_t family;
  if (napi_get_value_uint32(env, argv[6], &family) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  uint32_t ttl;
  if (napi_get_value_uint32(env, argv[7], &ttl) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  req->data = (void *) ((uintptr_t) rid);

  int err;

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  uv_buf_t b = uv_buf_init(buf, buf_len);

  udx_socket_send_ttl(req, self, &b, 1, (const struct sockaddr *) &addr, ttl, on_udx_send);

  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_socket_close (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_socket_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  int err = udx_socket_close(self, on_udx_close);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_stream_init (napi_env env, napi_callback_info info) {
  napi_value argv[16];
  size_t argc = 16;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_napi_t *udx;
  size_t udx_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &udx, &udx_len))

  udx_napi_stream_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &self, &self_len))

  uint32_t id;
  if (napi_get_value_uint32(env, argv[2], &id) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  uint32_t framed;
  if (napi_get_value_uint32(env, argv[3], &framed) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  udx_stream_t *stream = (udx_stream_t *) self;

  self->mode = framed ? UDX_NAPI_FRAMED : UDX_NAPI_INTERACTIVE;

  self->frame_len = -1;

  self->read_buf = NULL;
  self->read_buf_head = NULL;
  self->read_buf_free = 0;

  self->udx = udx;
  self->env = env;
  napi_create_reference(env, argv[4], 1, &(self->ctx));
  napi_create_reference(env, argv[5], 1, &(self->on_data));
  napi_create_reference(env, argv[6], 1, &(self->on_end));
  napi_create_reference(env, argv[7], 1, &(self->on_drain));
  napi_create_reference(env, argv[8], 1, &(self->on_ack));
  napi_create_reference(env, argv[9], 1, &(self->on_send));
  napi_create_reference(env, argv[10], 1, &(self->on_message));
  napi_create_reference(env, argv[11], 1, &(self->on_close));
  napi_create_reference(env, argv[12], 1, &(self->on_firewall));
  napi_create_reference(env, argv[13], 1, &(self->on_remote_changed));
  napi_create_reference(env, argv[14], 1, &(self->realloc_data));
  napi_create_reference(env, argv[15], 1, &(self->realloc_message));

  int err = udx_stream_init((udx_t *) udx, stream, id, on_udx_stream_close, on_udx_stream_finalize);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  udx_stream_firewall(stream, on_udx_stream_firewall);
  udx_stream_write_resume(stream, on_udx_stream_drain);

  return NULL;
}

napi_value
udx_napi_stream_set_seq (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  uint32_t seq;
  if (napi_get_value_uint32(env, argv[1], &seq) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  int err = udx_stream_set_seq(stream, seq);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_stream_set_ack (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  uint32_t ack;
  if (napi_get_value_uint32(env, argv[1], &ack) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  int err = udx_stream_set_ack(stream, ack);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_stream_set_mode (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_napi_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  uint32_t mode;
  if (napi_get_value_uint32(env, argv[1], &mode) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  stream->mode = mode;

  return NULL;
}

napi_value
udx_napi_stream_recv_start (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_napi_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  char *read_buf;
  size_t read_buf_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &read_buf, &read_buf_len))

  stream->read_buf = read_buf;
  stream->read_buf_head = read_buf;
  stream->read_buf_free = read_buf_len;

  udx_stream_read_start((udx_stream_t *) stream, on_udx_stream_read);
  udx_stream_recv_start((udx_stream_t *) stream, on_udx_stream_recv);

  return NULL;
}

napi_value
udx_napi_stream_connect (napi_env env, napi_callback_info info) {
  napi_value argv[6];
  size_t argc = 6;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  udx_socket_t *socket;
  size_t socket_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &socket, &socket_len))

  uint32_t remote_id;
  if (napi_get_value_uint32(env, argv[2], &remote_id) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  uint32_t port;
  if (napi_get_value_uint32(env, argv[3], &port) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  char ip[INET6_ADDRSTRLEN];
  size_t ip_len;
  if (napi_get_value_string_utf8(env, argv[4], (char *) &ip, INET6_ADDRSTRLEN, &ip_len) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected string");
    return NULL;
  }

  uint32_t family;
  if (napi_get_value_uint32(env, argv[5], &family) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  int err;

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  err = udx_stream_connect(stream, socket, remote_id, (const struct sockaddr *) &addr);

  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_stream_change_remote (napi_env env, napi_callback_info info) {
  napi_value argv[6];
  size_t argc = 6;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  udx_socket_t *socket;
  size_t socket_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &socket, &socket_len))

  uint32_t remote_id;
  if (napi_get_value_uint32(env, argv[2], &remote_id) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  uint32_t port;
  if (napi_get_value_uint32(env, argv[3], &port) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  char ip[INET6_ADDRSTRLEN];
  size_t ip_len;
  if (napi_get_value_string_utf8(env, argv[4], (char *) &ip, INET6_ADDRSTRLEN, &ip_len) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected string");
    return NULL;
  }

  uint32_t family;
  if (napi_get_value_uint32(env, argv[5], &family) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  int err;

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  err = udx_stream_change_remote(stream, socket, remote_id, (const struct sockaddr *) &addr, on_udx_stream_remote_changed);

  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_stream_relay_to (napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  udx_stream_t *destination;
  size_t destination_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &destination, &destination_len))

  int err = udx_stream_relay_to(stream, destination);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_stream_send (napi_env env, napi_callback_info info) {
  napi_value argv[4];
  size_t argc = 4;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  udx_stream_send_t *req;
  size_t req_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &req, &req_len))

  uint32_t rid;
  if (napi_get_value_uint32(env, argv[2], &rid) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  char *buf;
  size_t buf_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[3], (void **) &buf, &buf_len))

  req->data = (void *) ((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  int err = udx_stream_send(req, stream, &b, 1, on_udx_stream_send);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, err, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_stream_write (napi_env env, napi_callback_info info) {
  napi_value argv[4];
  size_t argc = 4;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  udx_stream_write_t *req;
  size_t req_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &req, &req_len))

  uint32_t rid;
  if (napi_get_value_uint32(env, argv[2], &rid) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  char *buf;
  size_t buf_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[3], (void **) &buf, &buf_len))

  req->data = (void *) ((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  int err = udx_stream_write(req, stream, &b, 1, on_udx_stream_ack);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, err, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_stream_writev (napi_env env, napi_callback_info info) {
  napi_value argv[4];
  size_t argc = 4;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  udx_stream_write_t *req;
  size_t req_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &req, &req_len))

  uint32_t rid;
  if (napi_get_value_uint32(env, argv[2], &rid) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  napi_value buffers = argv[3];

  req->data = (void *) ((uintptr_t) rid);

  uint32_t len;
  napi_get_array_length(env, buffers, &len);
  uv_buf_t *batch = malloc(sizeof(uv_buf_t) * len);

  napi_value element;
  for (uint32_t i = 0; i < len; i++) {
    napi_get_element(env, buffers, i, &element);

    char *buf;
    size_t buf_len;
    NAPI_STATUS_THROWS(napi_get_buffer_info(env, element, (void **) &buf, &buf_len))

    batch[i] = uv_buf_init(buf, buf_len);
  }

  int err = udx_stream_write(req, stream, batch, len, on_udx_stream_ack);
  free(batch);

  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, err, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_stream_write_sizeof (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  uint32_t bufs;
  if (napi_get_value_uint32(env, argv[0], &bufs) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, udx_stream_write_sizeof(bufs), &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_stream_write_end (napi_env env, napi_callback_info info) {
  napi_value argv[4];
  size_t argc = 4;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  udx_stream_write_t *req;
  size_t req_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[1], (void **) &req, &req_len))

  uint32_t rid;
  if (napi_get_value_uint32(env, argv[2], &rid) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  char *buf;
  size_t buf_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[3], (void **) &buf, &buf_len))

  req->data = (void *) ((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  int err = udx_stream_write_end(req, stream, &b, 1, on_udx_stream_ack);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, err, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_stream_destroy (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_stream_t *stream;
  size_t stream_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &stream, &stream_len))

  int err = udx_stream_destroy(stream);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  napi_value return_uint32;
  NAPI_STATUS_THROWS(napi_create_uint32(env, err, &return_uint32))
  return return_uint32;
}

napi_value
udx_napi_lookup (napi_env env, napi_callback_info info) {
  napi_value argv[5];
  size_t argc = 5;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_napi_lookup_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  size_t host_size = 0;
  NAPI_STATUS_THROWS(napi_get_value_string_utf8(env, argv[1], NULL, 0, &host_size))
  char *host = (char *) malloc((host_size + 1) * sizeof(char));
  size_t host_len;
  NAPI_STATUS_THROWS(napi_get_value_string_utf8(env, argv[1], host, host_size + 1, &host_len))
  host[host_size] = '\0';

  uint32_t family;
  if (napi_get_value_uint32(env, argv[2], &family) != napi_ok) {
    napi_throw_error(env, "EINVAL", "Expected unsigned number");
    return NULL;
  }

  udx_lookup_t *lookup = (udx_lookup_t *) self;

  uv_loop_t *loop;
  napi_get_uv_event_loop(env, &loop);

  self->host = host;
  self->env = env;
  napi_create_reference(env, argv[3], 1, &(self->ctx));
  napi_create_reference(env, argv[4], 1, &(self->on_lookup));

  int flags = 0;

  if (family == 4) flags |= UDX_LOOKUP_FAMILY_IPV4;
  if (family == 6) flags |= UDX_LOOKUP_FAMILY_IPV6;

  int err = udx_lookup(loop, lookup, host, flags, on_udx_lookup);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_interface_event_init (napi_env env, napi_callback_info info) {
  napi_value argv[4];
  size_t argc = 4;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_napi_interface_event_t *self;
  size_t self_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &self, &self_len))

  udx_interface_event_t *event = (udx_interface_event_t *) self;

  uv_loop_t *loop;
  napi_get_uv_event_loop(env, &loop);

  self->env = env;
  napi_create_reference(env, argv[1], 1, &(self->ctx));
  napi_create_reference(env, argv[2], 1, &(self->on_event));
  napi_create_reference(env, argv[3], 1, &(self->on_close));

  int err = udx_interface_event_init(loop, event);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  err = udx_interface_event_start(event, on_udx_interface_event, 5000);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_interface_event_start (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_interface_event_t *event;
  size_t event_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &event, &event_len))

  int err = udx_interface_event_start(event, on_udx_interface_event, 5000);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_interface_event_stop (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_interface_event_t *event;
  size_t event_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &event, &event_len))

  int err = udx_interface_event_stop(event);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_interface_event_close (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_interface_event_t *event;
  size_t event_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &event, &event_len))

  int err = udx_interface_event_close(event, on_udx_interface_event_close);
  if (err < 0) {
    napi_throw_error(env, uv_err_name(err), uv_strerror(err));
    return NULL;
  }

  return NULL;
}

napi_value
udx_napi_interface_event_get_addrs (napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

  udx_interface_event_t *event;
  size_t event_len;
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, argv[0], (void **) &event, &event_len))

  char ip[INET6_ADDRSTRLEN];
  int family = 0;

  napi_value napi_result;
  napi_create_array(env, &napi_result);

  for (int i = 0, j = 0; i < event->addrs_len; i++) {
    uv_interface_address_t addr = event->addrs[i];

    if (addr.address.address4.sin_family == AF_INET) {
      uv_ip4_name(&addr.address.address4, ip, sizeof(ip));
      family = 4;
    } else if (addr.address.address4.sin_family == AF_INET6) {
      uv_ip6_name(&addr.address.address6, ip, sizeof(ip));
      family = 6;
    } else {
      continue;
    }

    napi_value napi_item;
    napi_create_object(env, &napi_item);
    napi_set_element(env, napi_result, j++, napi_item);

    napi_value napi_name;
    napi_create_string_utf8(env, addr.name, NAPI_AUTO_LENGTH, &napi_name);
    napi_set_named_property(env, napi_item, "name", napi_name);

    napi_value napi_ip;
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &napi_ip);
    napi_set_named_property(env, napi_item, "host", napi_ip);

    napi_value napi_family;
    napi_create_uint32(env, family, &napi_family);
    napi_set_named_property(env, napi_item, "family", napi_family);

    napi_value napi_internal;
    napi_get_boolean(env, addr.is_internal, &napi_internal);
    napi_set_named_property(env, napi_item, "internal", napi_internal);
  }

  return napi_result;
}

static void
napi_macros_init (napi_env env, napi_value exports);

static napi_value
napi_macros_init_wrap (napi_env env, napi_value exports) {
  napi_macros_init(env, exports);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, napi_macros_init_wrap)
static void
napi_macros_init (napi_env env, napi_value exports) {
  {
    napi_value UV_UDP_IPV6ONLY_uint32;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, UV_UDP_IPV6ONLY, &UV_UDP_IPV6ONLY_uint32))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "UV_UDP_IPV6ONLY", UV_UDP_IPV6ONLY_uint32))
  }

  {
    napi_value inflight_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.inflight);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &inflight_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_inflight", inflight_offsetof))
  }
  {
    napi_value mtu_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.mtu);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &mtu_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_mtu", mtu_offsetof))
  }
  {
    napi_value cwnd_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.cwnd);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &cwnd_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_cwnd", cwnd_offsetof))
  }
  {
    napi_value srtt_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.srtt);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &srtt_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_srtt", srtt_offsetof))
  }
  {
    napi_value bytes_rx_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.bytes_rx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &bytes_rx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_bytes_rx", bytes_rx_offsetof))
  }
  {
    napi_value packets_rx_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.packets_rx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &packets_rx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_packets_rx", packets_rx_offsetof))
  }
  {
    napi_value bytes_tx_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.bytes_tx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &bytes_tx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_bytes_tx", bytes_tx_offsetof))
  }
  {
    napi_value packets_tx_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.packets_tx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &packets_tx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_packets_tx", packets_tx_offsetof))
  }
  {
    napi_value rto_count_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.rto_count);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &rto_count_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_rto_count", rto_count_offsetof))
  }
  {
    napi_value retransmit_count_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.retransmit_count);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &retransmit_count_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_retransmit_count", retransmit_count_offsetof))
  }
  {
    napi_value fast_recovery_count_offsetof;
    udx_stream_t tmp;
    void *ptr = &(tmp.fast_recovery_count);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &fast_recovery_count_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_stream_t_fast_recovery_count", fast_recovery_count_offsetof))
  }

  {
    napi_value bytes_rx_offsetof;
    udx_socket_t tmp;
    void *ptr = &(tmp.bytes_rx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &bytes_rx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_socket_t_bytes_rx", bytes_rx_offsetof))
  }
  {
    napi_value packets_rx_offsetof;
    udx_socket_t tmp;
    void *ptr = &(tmp.packets_rx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &packets_rx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_socket_t_packets_rx", packets_rx_offsetof))
  }
  {
    napi_value bytes_tx_offsetof;
    udx_socket_t tmp;
    void *ptr = &(tmp.bytes_tx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &bytes_tx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_socket_t_bytes_tx", bytes_tx_offsetof))
  }
  {
    napi_value packets_tx_offsetof;
    udx_socket_t tmp;
    void *ptr = &(tmp.packets_tx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &packets_tx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_socket_t_packets_tx", packets_tx_offsetof))
  }

  {
    napi_value bytes_rx_offsetof;
    udx_t tmp;
    void *ptr = &(tmp.bytes_rx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &bytes_rx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_t_bytes_rx", bytes_rx_offsetof))
  }
  {
    napi_value packets_rx_offsetof;
    udx_t tmp;
    void *ptr = &(tmp.packets_rx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &packets_rx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_t_packets_rx", packets_rx_offsetof))
  }
  {
    napi_value bytes_tx_offsetof;
    udx_t tmp;
    void *ptr = &(tmp.bytes_tx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &bytes_tx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_t_bytes_tx", bytes_tx_offsetof))
  }
  {
    napi_value packets_tx_offsetof;
    udx_t tmp;
    void *ptr = &(tmp.packets_tx);
    void *ptr_base = &tmp;
    int offset = (char *) ptr - (char *) ptr_base;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, offset, &packets_tx_offsetof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "offsetof_udx_t_packets_tx", packets_tx_offsetof))
  }

  {
    napi_value udx_napi_t_sizeof;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, sizeof(udx_napi_t), &udx_napi_t_sizeof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "sizeof_udx_napi_t", udx_napi_t_sizeof))
  }
  {
    napi_value udx_napi_socket_t_sizeof;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, sizeof(udx_napi_socket_t), &udx_napi_socket_t_sizeof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "sizeof_udx_napi_socket_t", udx_napi_socket_t_sizeof))
  }
  {
    napi_value udx_napi_stream_t_sizeof;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, sizeof(udx_napi_stream_t), &udx_napi_stream_t_sizeof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "sizeof_udx_napi_stream_t", udx_napi_stream_t_sizeof))
  }
  {
    napi_value udx_napi_lookup_t_sizeof;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, sizeof(udx_napi_lookup_t), &udx_napi_lookup_t_sizeof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "sizeof_udx_napi_lookup_t", udx_napi_lookup_t_sizeof))
  }
  {
    napi_value udx_napi_interface_event_t_sizeof;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, sizeof(udx_napi_interface_event_t), &udx_napi_interface_event_t_sizeof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "sizeof_udx_napi_interface_event_t", udx_napi_interface_event_t_sizeof))
  }

  {
    napi_value udx_socket_send_t_sizeof;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, sizeof(udx_socket_send_t), &udx_socket_send_t_sizeof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "sizeof_udx_socket_send_t", udx_socket_send_t_sizeof))
  }
  {
    napi_value udx_stream_send_t_sizeof;
    NAPI_STATUS_THROWS_VOID(napi_create_uint32(env, sizeof(udx_stream_send_t), &udx_stream_send_t_sizeof))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "sizeof_udx_stream_send_t", udx_stream_send_t_sizeof))
  }

  {
    napi_value udx_napi_init_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_init, NULL, &udx_napi_init_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_init", udx_napi_init_fn))
  }

  {
    napi_value udx_napi_socket_init_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_init, NULL, &udx_napi_socket_init_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_init", udx_napi_socket_init_fn))
  }
  {
    napi_value udx_napi_socket_bind_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_bind, NULL, &udx_napi_socket_bind_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_bind", udx_napi_socket_bind_fn))
  }
  {
    napi_value udx_napi_socket_set_ttl_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_set_ttl, NULL, &udx_napi_socket_set_ttl_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_set_ttl", udx_napi_socket_set_ttl_fn))
  }
  {
    napi_value udx_napi_socket_get_recv_buffer_size_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_get_recv_buffer_size, NULL, &udx_napi_socket_get_recv_buffer_size_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_get_recv_buffer_size", udx_napi_socket_get_recv_buffer_size_fn))
  }
  {
    napi_value udx_napi_socket_set_recv_buffer_size_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_set_recv_buffer_size, NULL, &udx_napi_socket_set_recv_buffer_size_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_set_recv_buffer_size", udx_napi_socket_set_recv_buffer_size_fn))
  }
  {
    napi_value udx_napi_socket_get_send_buffer_size_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_get_send_buffer_size, NULL, &udx_napi_socket_get_send_buffer_size_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_get_send_buffer_size", udx_napi_socket_get_send_buffer_size_fn))
  }
  {
    napi_value udx_napi_socket_set_send_buffer_size_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_set_send_buffer_size, NULL, &udx_napi_socket_set_send_buffer_size_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_set_send_buffer_size", udx_napi_socket_set_send_buffer_size_fn))
  }
  {
    napi_value udx_napi_socket_send_ttl_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_send_ttl, NULL, &udx_napi_socket_send_ttl_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_send_ttl", udx_napi_socket_send_ttl_fn))
  }
  {
    napi_value udx_napi_socket_close_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_socket_close, NULL, &udx_napi_socket_close_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_socket_close", udx_napi_socket_close_fn))
  }

  {
    napi_value udx_napi_stream_init_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_init, NULL, &udx_napi_stream_init_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_init", udx_napi_stream_init_fn))
  }
  {
    napi_value udx_napi_stream_set_seq_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_set_seq, NULL, &udx_napi_stream_set_seq_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_set_seq", udx_napi_stream_set_seq_fn))
  }
  {
    napi_value udx_napi_stream_set_ack_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_set_ack, NULL, &udx_napi_stream_set_ack_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_set_ack", udx_napi_stream_set_ack_fn))
  }
  {
    napi_value udx_napi_stream_set_mode_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_set_mode, NULL, &udx_napi_stream_set_mode_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_set_mode", udx_napi_stream_set_mode_fn))
  }
  {
    napi_value udx_napi_stream_connect_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_connect, NULL, &udx_napi_stream_connect_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_connect", udx_napi_stream_connect_fn))
  }
  {
    napi_value udx_napi_stream_change_remote_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_change_remote, NULL, &udx_napi_stream_change_remote_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_change_remote", udx_napi_stream_change_remote_fn))
  }
  {
    napi_value udx_napi_stream_relay_to_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_relay_to, NULL, &udx_napi_stream_relay_to_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_relay_to", udx_napi_stream_relay_to_fn))
  }
  {
    napi_value udx_napi_stream_send_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_send, NULL, &udx_napi_stream_send_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_send", udx_napi_stream_send_fn))
  }
  {
    napi_value udx_napi_stream_recv_start_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_recv_start, NULL, &udx_napi_stream_recv_start_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_recv_start", udx_napi_stream_recv_start_fn))
  }
  {
    napi_value udx_napi_stream_write_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_write, NULL, &udx_napi_stream_write_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_write", udx_napi_stream_write_fn))
  }
  {
    napi_value udx_napi_stream_writev_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_writev, NULL, &udx_napi_stream_writev_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_writev", udx_napi_stream_writev_fn))
  }
  {
    napi_value udx_napi_stream_write_sizeof_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_write_sizeof, NULL, &udx_napi_stream_write_sizeof_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_write_sizeof", udx_napi_stream_write_sizeof_fn))
  }
  {
    napi_value udx_napi_stream_write_end_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_write_end, NULL, &udx_napi_stream_write_end_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_write_end", udx_napi_stream_write_end_fn))
  }
  {
    napi_value udx_napi_stream_destroy_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_stream_destroy, NULL, &udx_napi_stream_destroy_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_stream_destroy", udx_napi_stream_destroy_fn))
  }

  {
    napi_value udx_napi_lookup_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_lookup, NULL, &udx_napi_lookup_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_lookup", udx_napi_lookup_fn))
  }

  {
    napi_value udx_napi_interface_event_init_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_interface_event_init, NULL, &udx_napi_interface_event_init_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_interface_event_init", udx_napi_interface_event_init_fn))
  }
  {
    napi_value udx_napi_interface_event_start_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_interface_event_start, NULL, &udx_napi_interface_event_start_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_interface_event_start", udx_napi_interface_event_start_fn))
  }
  {
    napi_value udx_napi_interface_event_stop_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_interface_event_stop, NULL, &udx_napi_interface_event_stop_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_interface_event_stop", udx_napi_interface_event_stop_fn))
  }
  {
    napi_value udx_napi_interface_event_close_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_interface_event_close, NULL, &udx_napi_interface_event_close_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_interface_event_close", udx_napi_interface_event_close_fn))
  }
  {
    napi_value udx_napi_interface_event_get_addrs_fn;
    NAPI_STATUS_THROWS_VOID(napi_create_function(env, NULL, 0, udx_napi_interface_event_get_addrs, NULL, &udx_napi_interface_event_get_addrs_fn))
    NAPI_STATUS_THROWS_VOID(napi_set_named_property(env, exports, "udx_napi_interface_event_get_addrs", udx_napi_interface_event_get_addrs_fn))
  }
}
