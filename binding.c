// #include <node_api.h>
// #include <inttypes.h>
#include <udx.h>
#include <bare.h>
#include <js.h>
#include <stdlib.h>
#include <string.h>

#ifndef NAPI_AUTO_LENGTH // TODO: remove
#define NAPI_AUTO_LENGTH ((size_t) - 1)
#endif

#define UDX_NAPI_INTERACTIVE     0
#define UDX_NAPI_NON_INTERACTIVE 1
#define UDX_NAPI_FRAMED          2

typedef struct {
  udx_t udx;

  char *read_buf;
  size_t read_buf_free;

  // napi_async_cleanup_hook_handle teardown;
  js_deferred_teardown_t *teardown;
  bool exiting;
  bool has_teardown;
} udx_napi_t;

typedef struct {
  udx_socket_t socket;
  udx_napi_t *udx;

  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *on_send;
  js_ref_t *on_message;
  js_ref_t *on_close;
  js_ref_t *realloc_message;
} udx_napi_socket_t;

typedef struct {
  udx_stream_t stream;
  udx_napi_t *udx;

  int mode;

  char *read_buf;
  char *read_buf_head;
  size_t read_buf_free;

  ssize_t frame_len;

  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *on_data;
  js_ref_t *on_end;
  js_ref_t *on_drain;
  js_ref_t *on_ack;
  js_ref_t *on_send;
  js_ref_t *on_message;
  js_ref_t *on_close;
  js_ref_t *on_firewall;
  js_ref_t *on_remote_changed;
  js_ref_t *realloc_data;
  js_ref_t *realloc_message;
} udx_napi_stream_t;

typedef struct {
  udx_lookup_t handle;
  udx_napi_t *udx;

  char *host;

  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *on_lookup;
} udx_napi_lookup_t;

typedef struct {
  udx_interface_event_t handle;
  udx_napi_t *udx;

  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *on_event;
  js_ref_t *on_close;
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

/**
 * Forward exceptions to uncaught handler
 * if exception occured on js-callback /w checkpoint
 */
static inline void
forward_uncaught (js_env_t *env, int err) {
  if (err == 0) return;

  assert(err == js_pending_exception || err == js_uncaught_exception);

  js_value_t *fatal_exception;
  err = js_get_and_clear_last_exception(env, &fatal_exception);
  assert(err == 0);

  err = js_fatal_exception(env, fatal_exception);
  assert(err == 0);
}

static void
on_udx_send (udx_socket_send_t *req, int status) {
  int err;
  udx_napi_socket_t *n = (udx_napi_socket_t *) req->socket;
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_send, &callback);
  assert(err == 0);

  js_value_t *argv[2];
  err = js_create_int32(env, (uintptr_t) req->data, &(argv[0]));
  assert(err == 0);
  err = js_create_int32(env, status, &(argv[1]));
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 2, argv, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_message (udx_socket_t *self, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  udx_napi_socket_t *n = (udx_napi_socket_t *) self;
  if (n->udx->exiting) return;

  int err;
  int port = 0;
  char ip[INET6_ADDRSTRLEN];
  int family = 0;
  parse_address((struct sockaddr *) from, ip, INET6_ADDRSTRLEN, &port, &family);

  if (buf->len > n->udx->read_buf_free) return;

  memcpy(n->udx->read_buf, buf->base, buf->len);

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_message, &callback);
  assert(err == 0);

  js_value_t *argv[4];
  err = js_create_uint32(env, read_len, &(argv[0]));
  assert(err == 0);
  err = js_create_uint32(env, port, &(argv[1]));
  assert(err == 0);
  err = js_create_string_utf8(env, (utf8_t *) ip, NAPI_AUTO_LENGTH, &(argv[2]));
  assert(err == 0);
  err = js_create_uint32(env, family, &(argv[3]));
  assert(err == 0);

  js_value_t *res;

  err = js_call_function_with_checkpoint(env, ctx, callback, 4, argv, &res);
  forward_uncaught(env, err);

  if (err == 0) {
    err = js_get_arraybuffer_info(env, res, (void **) &(n->udx->read_buf), &(n->udx->read_buf_free));
    assert(err == 0);
  } else {

    // avoid reentry
    if (!(n->udx->exiting)) {
      js_env_t *env = n->env;

      js_handle_scope_t *scope;
      err = js_open_handle_scope(env, &scope);
      assert(err == 0);

      js_value_t *ctx;
      err = js_get_reference_value(env, n->ctx, &ctx);
      assert(err == 0);

      js_value_t *callback;
      err = js_get_reference_value(env, n->realloc_message, &callback);
      assert(err == 0);

      err = js_call_function_with_checkpoint(env, ctx, callback, 0, NULL, &res);
      forward_uncaught(env, err);

      if (err == 0) {
        err = js_get_arraybuffer_info(env, res, (void **) &(n->udx->read_buf), &(n->udx->read_buf_free));
        assert(err == 0);
      }

      err = js_close_handle_scope(env, scope);
      assert(err == 0);
    }
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_close (udx_socket_t *self) {
  udx_napi_socket_t *n = (udx_napi_socket_t *) self;
  int err;
  js_env_t *env = n->env;

  if (!(n->udx->exiting)) {
    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    js_value_t *ctx;
    err = js_get_reference_value(env, n->ctx, &ctx);
    assert(err == 0);

    js_value_t *callback;
    err = js_get_reference_value(env, n->on_close, &callback);
    assert(err == 0);

    err = js_call_function_with_checkpoint(env, ctx, callback, 0, NULL, NULL);
    forward_uncaught(env, err);

    js_close_handle_scope(env, scope);
  }

  err = js_delete_reference(env, n->on_send);
  assert(err == 0);
  err = js_delete_reference(env, n->on_message);
  assert(err == 0);
  err = js_delete_reference(env, n->on_close);
  assert(err == 0);
  err = js_delete_reference(env, n->realloc_message);
  assert(err == 0);
  err = js_delete_reference(env, n->ctx);
  assert(err == 0);
}

static void
on_udx_teardown (js_deferred_teardown_t *handle, void *data) {
  udx_napi_t *self = (udx_napi_t *) data;
  udx_t *udx = (udx_t *) data;

  self->exiting = true;
  udx_teardown(udx);
}

static void
ensure_teardown (js_env_t *env, udx_napi_t *udx) {
  if (udx->has_teardown) return;
  udx->has_teardown = true;

  int err = js_add_deferred_teardown_callback(env, on_udx_teardown, (void *) udx, &(udx->teardown));
  if (err != 0) abort();
}

static void
on_udx_stream_end (udx_stream_t *stream) {
  int err;
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  if (n->udx->exiting) return;

  size_t read = n->read_buf_head - n->read_buf;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_end, &callback);
  assert(err == 0);

  js_value_t *argv[1];
  err = js_create_uint32(env, read, &(argv[0]));
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 1, argv, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len == UV_EOF) return on_udx_stream_end(stream);

  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  if (n->udx->exiting) return;

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

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  int err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_data, &callback);
  assert(err == 0);

  js_value_t *argv[1];
  err = js_create_uint32(env, read, &(argv[0]));
  assert(err == 0);

  js_value_t *res;

  err = js_call_function_with_checkpoint(env, ctx, callback, 1, argv, &res);
  if (err == 0) {
    err = js_get_arraybuffer_info(env, res, (void **) &(n->read_buf), &(n->read_buf_free));
    assert(err == 0);
    n->read_buf_head = n->read_buf;
  } else {
    forward_uncaught(env, err);

    // avoid re-entry
    if (!(n->udx->exiting)) {
      js_handle_scope_t *scope;
      err = js_open_handle_scope(env, &scope);
      assert(err == 0);

      js_value_t *ctx;
      err = js_get_reference_value(env, n->ctx, &ctx);
      assert(err == 0);

      js_value_t *callback;
      err = js_get_reference_value(env, n->realloc_data, &callback);
      assert(err == 0);

      err = js_call_function_with_checkpoint(env, ctx, callback, 0, NULL, &res);
      forward_uncaught(env, err);

      err = js_get_arraybuffer_info(env, res, (void **) &(n->read_buf), &(n->read_buf_free));
      assert(err == 0);
      n->read_buf_head = n->read_buf;

      err = js_close_handle_scope(env, scope);
      assert(err == 0);
    }
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_drain (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  int err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_drain, &callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 0, NULL, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_ack (udx_stream_write_t *req, int status, int unordered) {
  int err = 0;
  udx_napi_stream_t *n = (udx_napi_stream_t *) req->stream;
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_ack, &callback);
  assert(err == 0);

  js_value_t *argv[1];
  err = js_create_uint32(env, (uintptr_t) req->data, &(argv[0]));
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 1, argv, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_send (udx_stream_send_t *req, int status) {
  int err;
  udx_napi_stream_t *n = (udx_napi_stream_t *) req->stream;
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_send, &callback);
  assert(err == 0);

  js_value_t *argv[2];
  err = js_create_int32(env, (uintptr_t) req->data, &(argv[0]));
  assert(err == 0);
  err = js_create_int32(env, status, &(argv[1]));
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 2, argv, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_recv (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  int err;
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  if (n->udx->exiting) return;

  if (buf->len > n->udx->read_buf_free) return;

  memcpy(n->udx->read_buf, buf->base, buf->len);

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_message, &callback);
  assert(err == 0);

  js_value_t *argv[1];
  err = js_create_uint32(env, read_len, &(argv[0]));
  assert(err == 0);

  js_value_t *res;
  err = js_call_function_with_checkpoint(env, ctx, callback, 1, argv, &res);
  if (err == 0) {
    err = js_get_arraybuffer_info(env, res, (void **) &(n->udx->read_buf), &(n->udx->read_buf_free));
    assert(err == 0);
  } else {
    forward_uncaught(env, err);
    // avoid re-entry
    if (!(n->udx->exiting)) {
      js_handle_scope_t *scope;
      err = js_open_handle_scope(env, &scope);
      assert(err == 0);

      js_value_t *ctx;
      err = js_get_reference_value(env, n->ctx, &ctx);
      assert(err == 0);

      js_value_t *callback;
      err = js_get_reference_value(env, n->realloc_message, &callback);
      assert(err == 0);

      err = js_call_function_with_checkpoint(env, ctx, callback, 0, NULL, &res);
      forward_uncaught(env, err);

      err = js_get_arraybuffer_info(env, res, (void **) &(n->udx->read_buf), &(n->udx->read_buf_free));
      assert(err == 0);

      err = js_close_handle_scope(env, scope);
      assert(err == 0);
    }
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_finalize (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  int err = js_delete_reference(n->env, n->on_data);
  assert(err == 0);
  err = js_delete_reference(n->env, n->on_end);
  assert(err == 0);
  err = js_delete_reference(n->env, n->on_drain);
  assert(err == 0);
  err = js_delete_reference(n->env, n->on_ack);
  assert(err == 0);
  err = js_delete_reference(n->env, n->on_send);
  assert(err == 0);
  err = js_delete_reference(n->env, n->on_message);
  assert(err == 0);
  err = js_delete_reference(n->env, n->on_close);
  assert(err == 0);
  err = js_delete_reference(n->env, n->on_firewall);
  assert(err == 0);
  err = js_delete_reference(n->env, n->on_remote_changed);
  assert(err == 0);
  err = js_delete_reference(n->env, n->realloc_data);
  assert(err == 0);
  err = js_delete_reference(n->env, n->realloc_message);
  assert(err == 0);
  err = js_delete_reference(n->env, n->ctx);
  assert(err == 0);
}

static void
on_udx_stream_close (udx_stream_t *stream, int status) {
  int err;
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_close, &callback);
  assert(err == 0);

  js_value_t *argv[1];

  if (status >= 0) {
    js_get_null(env, &(argv[0]));
  } else {
    js_value_t *code;
    js_value_t *msg;
    err = js_create_string_utf8(env, (utf8_t *) uv_err_name(status), NAPI_AUTO_LENGTH, &code);
    assert(err == 0);
    err = js_create_string_utf8(env, (utf8_t *) uv_strerror(status), NAPI_AUTO_LENGTH, &msg);
    assert(err == 0);
    err = js_create_error(env, code, msg, &(argv[0]));
    assert(err == 0);
  }

  err = js_call_function_with_checkpoint(env, ctx, callback, 1, argv, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
}

static int
on_udx_stream_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  int err;
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  udx_napi_socket_t *s = (udx_napi_socket_t *) socket;

  uint32_t fw = 1; // assume error means firewall it, whilst reporting the uncaught
  if (n->udx->exiting) return fw;

  int port = 0;
  char ip[INET6_ADDRSTRLEN];
  int family = 0;
  parse_address((struct sockaddr *) from, ip, INET6_ADDRSTRLEN, &port, &family);

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_firewall, &callback);
  assert(err == 0);

  js_value_t *res;
  js_value_t *argv[4];

  err = js_get_reference_value(env, s->ctx, &(argv[0]));
  assert(err == 0);
  err = js_create_uint32(env, port, &(argv[1]));
  assert(err == 0);
  err = js_create_string_utf8(env, (utf8_t *) ip, NAPI_AUTO_LENGTH, &(argv[2]));
  assert(err == 0);
  err = js_create_uint32(env, family, &(argv[3]));
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 4, argv, &res);
  if (err == 0) {
    err = js_get_value_uint32(env, res, &fw);
    assert(err == 0);
  } else {
    forward_uncaught(env, err);
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  return fw;
}

static void
on_udx_stream_remote_changed (udx_stream_t *stream) {
  int err;
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_remote_changed, &callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 0, NULL, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_lookup (udx_lookup_t *lookup, int status, const struct sockaddr *addr, int addr_len) {
  int err;
  udx_napi_lookup_t *n = (udx_napi_lookup_t *) lookup;
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  char ip[INET6_ADDRSTRLEN] = "";
  int family = 0;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, n->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, n->on_lookup, &callback);
  assert(err == 0);

  if (status >= 0) {
    if (addr->sa_family == AF_INET) {
      uv_ip4_name((struct sockaddr_in *) addr, ip, addr_len);
      family = 4;
    } else if (addr->sa_family == AF_INET6) {
      uv_ip6_name((struct sockaddr_in6 *) addr, ip, addr_len);
      family = 6;
    }

    js_value_t *argv[3];
    err = js_get_null(env, &(argv[0]));
    assert(err == 0);
    err = js_create_string_utf8(env, (utf8_t *) ip, NAPI_AUTO_LENGTH, &(argv[1]));
    assert(err == 0);
    err = js_create_uint32(env, family, &(argv[2]));
    assert(err == 0);

    err = js_call_function_with_checkpoint(env, ctx, callback, 3, argv, NULL);
    forward_uncaught(env, err);
  } else {
    js_value_t *argv[1];
    js_value_t *code;
    js_value_t *msg;
    err = js_create_string_utf8(env, (utf8_t *) uv_err_name(status), NAPI_AUTO_LENGTH, &code);
    assert(err == 0);
    err = js_create_string_utf8(env, (utf8_t *) uv_strerror(status), NAPI_AUTO_LENGTH, &msg);
    assert(err == 0);
    err = js_create_error(env, code, msg, &(argv[0]));
    assert(err == 0);

    err = js_call_function_with_checkpoint(env, ctx, callback, 1, argv, NULL);
    forward_uncaught(env, err);
  }

  free(n->host);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  err = js_delete_reference(n->env, n->on_lookup);
  assert(err == 0);
  err = js_delete_reference(n->env, n->ctx);
  assert(err == 0);
}

static void
on_udx_interface_event (udx_interface_event_t *handle, int status) {
  udx_napi_interface_event_t *e = (udx_napi_interface_event_t *) handle;
  if (e->udx->exiting) return;

  js_env_t *env = e->env;

  js_handle_scope_t *scope;
  int err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, e->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, e->on_event, &callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 0, NULL, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_interface_event_close (udx_interface_event_t *handle) {
  udx_napi_interface_event_t *e = (udx_napi_interface_event_t *) handle;
  if (e->udx->exiting) return;

  js_env_t *env = e->env;

  js_handle_scope_t *scope;
  int err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, e->ctx, &ctx);
  assert(err == 0);

  js_value_t *callback;
  err = js_get_reference_value(env, e->on_close, &callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, ctx, callback, 0, NULL, NULL);
  forward_uncaught(env, err);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  err = js_delete_reference(env, e->on_event);
  assert(err == 0);
  err = js_delete_reference(env, e->on_close);
  assert(err == 0);
  err = js_delete_reference(env, e->ctx);
  assert(err == 0);
}

static void
on_udx_idle (udx_t *u) {
  udx_napi_t *self = (udx_napi_t *) u;
  if (!self->has_teardown) return;
  self->has_teardown = false;

  int err = js_finish_deferred_teardown_callback(self->teardown);
  if (err != 0) abort();
}

js_value_t *
udx_napi_init (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_napi_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  char *read_buf;
  size_t read_buf_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &read_buf, &read_buf_len);
  assert(err == 0);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  err = udx_init(loop, &(self->udx), on_udx_idle);
  assert(err == 0);

  self->read_buf = read_buf;
  self->read_buf_free = read_buf_len;
  self->exiting = false;
  self->has_teardown = false;

  return NULL;
}

js_value_t *
udx_napi_socket_init (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[7];
  size_t argc = 7;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_napi_t *udx;
  size_t udx_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &udx, &udx_len);
  assert(err == 0);

  udx_napi_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &self, &self_len);
  assert(err == 0);

  udx_socket_t *socket = (udx_socket_t *) self;

  self->udx = udx;
  self->env = env;
  err = js_create_reference(env, argv[2], 1, &(self->ctx));
  assert(err == 0);
  err = js_create_reference(env, argv[3], 1, &(self->on_send));
  assert(err == 0);
  err = js_create_reference(env, argv[4], 1, &(self->on_message));
  assert(err == 0);
  err = js_create_reference(env, argv[5], 1, &(self->on_close));
  assert(err == 0);
  err = js_create_reference(env, argv[6], 1, &(self->realloc_message));
  assert(err == 0);

  err = udx_socket_init((udx_t *) udx, socket, on_udx_close);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  ensure_teardown(env, udx);

  return NULL;
}

js_value_t *
udx_napi_socket_bind (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[5];
  size_t argc = 5;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  uint32_t port;
  err = js_get_value_uint32(env, argv[1], &port);
  assert(err == 0);

  char ip[INET6_ADDRSTRLEN];
  size_t ip_len;
  err = js_get_value_string_utf8(env, argv[2], (utf8_t *) &ip, INET6_ADDRSTRLEN, &ip_len);
  assert(err == 0);

  uint32_t family;
  err = js_get_value_uint32(env, argv[3], &family);
  assert(err == 0);

  uint32_t flags;
  err = js_get_value_uint32(env, argv[4], &flags);
  assert(err == 0);

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
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  err = udx_socket_bind(self, (struct sockaddr *) &addr, flags);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  // TODO: move the bottom stuff into another function, start, so error handling is easier

  struct sockaddr_storage name;

  // wont error in practice
  err = udx_socket_getsockname(self, (struct sockaddr *) &name, &addr_len);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
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
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, local_port, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_socket_set_ttl (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  uint32_t ttl;
  err = js_get_value_uint32(env, argv[1], &ttl);
  assert(err == 0);

  err = udx_socket_set_ttl(self, ttl);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_socket_set_membership (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[4];
  size_t argc = 4;

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *socket;
  size_t socket_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &socket, &socket_len);
  assert(err == 0);

  char mcast_addr[INET6_ADDRSTRLEN];
  size_t mcast_addr_len;
  err = js_get_value_string_utf8(env, argv[1], (utf8_t *) mcast_addr, INET6_ADDRSTRLEN, &mcast_addr_len);
  assert(err == 0);

  char iface_addr[INET6_ADDRSTRLEN];
  size_t iface_addr_len;
  err = js_get_value_string_utf8(env, argv[2], (utf8_t *) iface_addr, INET6_ADDRSTRLEN, &iface_addr_len);
  assert(err == 0);

  char *iface_param = iface_addr_len > 0 ? iface_addr : NULL;

  bool join; // true for join, false for leave
  err = js_get_value_bool(env, argv[3], &join);
  assert(err == 0);

  err = udx_socket_set_membership(socket, mcast_addr, iface_param, join ? UV_JOIN_GROUP : UV_LEAVE_GROUP);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_socket_get_recv_buffer_size (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  int size = 0;

  err = udx_socket_get_recv_buffer_size(self, &size);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, size, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_socket_set_recv_buffer_size (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  int32_t size;
  err = js_get_value_int32(env, argv[1], &size);
  assert(err == 0);

  err = udx_socket_set_recv_buffer_size(self, size);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_socket_get_send_buffer_size (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  int size = 0;

  err = udx_socket_get_send_buffer_size(self, &size);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, size, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_socket_set_send_buffer_size (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  int32_t size;
  err = js_get_value_int32(env, argv[1], &size);
  assert(err == 0);

  err = udx_socket_set_send_buffer_size(self, size);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, size, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_socket_send_ttl (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[8];
  size_t argc = 8;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  udx_socket_send_t *req;
  size_t req_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &req, &req_len);
  assert(err == 0);

  uint32_t rid;
  err = js_get_value_uint32(env, argv[2], &rid);
  assert(err == 0);

  char *buf;
  size_t buf_len;
  err = js_get_arraybuffer_info(env, argv[3], (void **) &buf, &buf_len);
  assert(err == 0);

  uint32_t port;
  err = js_get_value_uint32(env, argv[4], &port);
  assert(err == 0);

  char ip[INET6_ADDRSTRLEN];
  size_t ip_len;
  err = js_get_value_string_utf8(env, argv[5], (utf8_t *) &ip, INET6_ADDRSTRLEN, &ip_len);
  assert(err == 0);

  uint32_t family;
  err = js_get_value_uint32(env, argv[6], &family);
  assert(err == 0);

  uint32_t ttl;
  err = js_get_value_uint32(env, argv[7], &ttl);
  assert(err == 0);

  req->data = (void *) ((uintptr_t) rid);

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  uv_buf_t b = uv_buf_init(buf, buf_len);

  err = udx_socket_send_ttl(req, self, &b, 1, (const struct sockaddr *) &addr, ttl, on_udx_send);
  assert(err == 0); // previously unchecked

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);

    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_socket_close (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_socket_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &self, &self_len);
  assert(err == 0);

  err = udx_socket_close(self);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_stream_init (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[16];
  size_t argc = 16;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_napi_t *udx;
  size_t udx_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &udx, &udx_len);
  assert(err == 0);

  udx_napi_stream_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &self, &self_len);
  assert(err == 0);

  uint32_t id;
  err = js_get_value_uint32(env, argv[2], &id);
  assert(err == 0);

  uint32_t framed;
  err = js_get_value_uint32(env, argv[3], &framed);
  assert(err == 0);

  udx_stream_t *stream = (udx_stream_t *) self;

  self->mode = framed ? UDX_NAPI_FRAMED : UDX_NAPI_INTERACTIVE;

  self->frame_len = -1;

  self->read_buf = NULL;
  self->read_buf_head = NULL;
  self->read_buf_free = 0;

  self->udx = udx;
  self->env = env;
  err = js_create_reference(env, argv[4], 1, &(self->ctx));
  assert(err == 0);
  err = js_create_reference(env, argv[5], 1, &(self->on_data));
  assert(err == 0);
  err = js_create_reference(env, argv[6], 1, &(self->on_end));
  assert(err == 0);
  err = js_create_reference(env, argv[7], 1, &(self->on_drain));
  assert(err == 0);
  err = js_create_reference(env, argv[8], 1, &(self->on_ack));
  assert(err == 0);
  err = js_create_reference(env, argv[9], 1, &(self->on_send));
  assert(err == 0);
  err = js_create_reference(env, argv[10], 1, &(self->on_message));
  assert(err == 0);
  err = js_create_reference(env, argv[11], 1, &(self->on_close));
  assert(err == 0);
  err = js_create_reference(env, argv[12], 1, &(self->on_firewall));
  assert(err == 0);
  err = js_create_reference(env, argv[13], 1, &(self->on_remote_changed));
  assert(err == 0);
  err = js_create_reference(env, argv[14], 1, &(self->realloc_data));
  assert(err == 0);
  err = js_create_reference(env, argv[15], 1, &(self->realloc_message));
  assert(err == 0);

  err = udx_stream_init((udx_t *) udx, stream, id, on_udx_stream_close, on_udx_stream_finalize);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  udx_stream_firewall(stream, on_udx_stream_firewall);
  udx_stream_write_resume(stream, on_udx_stream_drain);

  ensure_teardown(env, udx);

  return NULL;
}

js_value_t *
udx_napi_stream_set_seq (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  uint32_t seq;
  err = js_get_value_uint32(env, argv[1], &seq);
  assert(err == 0);

  err = udx_stream_set_seq(stream, seq);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_stream_set_ack (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  uint32_t ack;
  err = js_get_value_uint32(env, argv[1], &ack);
  assert(err == 0);

  err = udx_stream_set_ack(stream, ack);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_stream_set_mode (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_napi_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  uint32_t mode;
  err = js_get_value_uint32(env, argv[1], &mode);
  assert(err == 0);

  stream->mode = mode;

  return NULL;
}

js_value_t *
udx_napi_stream_recv_start (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_napi_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  char *read_buf;
  size_t read_buf_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &read_buf, &read_buf_len);
  assert(err == 0);

  stream->read_buf = read_buf;
  stream->read_buf_head = read_buf;
  stream->read_buf_free = read_buf_len;

  err = udx_stream_read_start((udx_stream_t *) stream, on_udx_stream_read);
  assert(err == 0); // napi_unchecked
  err = udx_stream_recv_start((udx_stream_t *) stream, on_udx_stream_recv);
  assert(err == 0); // napi_unchecked

  return NULL;
}

js_value_t *
udx_napi_stream_connect (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[6];
  size_t argc = 6;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  udx_socket_t *socket;
  size_t socket_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &socket, &socket_len);
  assert(err == 0);

  uint32_t remote_id;
  err = js_get_value_uint32(env, argv[2], &remote_id);
  assert(err == 0);

  uint32_t port;
  err = js_get_value_uint32(env, argv[3], &port);
  assert(err == 0);

  char ip[INET6_ADDRSTRLEN];
  size_t ip_len;
  err = js_get_value_string_utf8(env, argv[4], (utf8_t *) &ip, INET6_ADDRSTRLEN, &ip_len);
  assert(err == 0);

  uint32_t family;
  err = js_get_value_uint32(env, argv[5], &family);
  assert(err == 0);

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  err = udx_stream_connect(stream, socket, remote_id, (const struct sockaddr *) &addr);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_stream_change_remote (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[6];
  size_t argc = 6;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  udx_socket_t *socket;
  size_t socket_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &socket, &socket_len);
  assert(err == 0);

  uint32_t remote_id;
  err = js_get_value_uint32(env, argv[2], &remote_id);
  assert(err == 0);

  uint32_t port;
  err = js_get_value_uint32(env, argv[3], &port);
  assert(err == 0);

  char ip[INET6_ADDRSTRLEN];
  size_t ip_len;
  err = js_get_value_string_utf8(env, argv[4], (utf8_t *) &ip, INET6_ADDRSTRLEN, &ip_len);
  assert(err == 0);

  uint32_t family;
  err = js_get_value_uint32(env, argv[5], &family);
  assert(err == 0);

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  err = udx_stream_change_remote(stream, socket, remote_id, (const struct sockaddr *) &addr, on_udx_stream_remote_changed);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_stream_relay_to (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[2];
  size_t argc = 2;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  udx_stream_t *destination;
  size_t destination_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &destination, &destination_len);
  assert(err == 0);

  err = udx_stream_relay_to(stream, destination);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_stream_send (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[4];
  size_t argc = 4;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  udx_stream_send_t *req;
  size_t req_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &req, &req_len);
  assert(err == 0);

  uint32_t rid;
  err = js_get_value_uint32(env, argv[2], &rid);
  assert(err == 0);

  char *buf;
  size_t buf_len;
  err = js_get_arraybuffer_info(env, argv[3], (void **) &buf, &buf_len);
  assert(err == 0);

  req->data = (void *) ((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  err = udx_stream_send(req, stream, &b, 1, on_udx_stream_send);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, err, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_stream_write (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[4];
  size_t argc = 4;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  udx_stream_write_t *req;
  size_t req_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &req, &req_len);
  assert(err == 0);

  uint32_t rid;
  err = js_get_value_uint32(env, argv[2], &rid);
  assert(err == 0);

  char *buf;
  size_t buf_len;
  err = js_get_arraybuffer_info(env, argv[3], (void **) &buf, &buf_len);
  assert(err == 0);

  req->data = (void *) ((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  err = udx_stream_write(req, stream, &b, 1, on_udx_stream_ack);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, err, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_stream_writev (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[4];
  size_t argc = 4;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  udx_stream_write_t *req;
  size_t req_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &req, &req_len);
  assert(err == 0);

  uint32_t rid;
  err = js_get_value_uint32(env, argv[2], &rid);
  assert(err == 0);

  js_value_t *buffers = argv[3];

  req->data = (void *) ((uintptr_t) rid);

  uint32_t len;
  err = js_get_array_length(env, buffers, &len);
  assert(err == 0);

  uv_buf_t *batch = malloc(sizeof(uv_buf_t) * len);

  js_value_t *element;
  for (uint32_t i = 0; i < len; i++) {
    err = js_get_element(env, buffers, i, &element);
    assert(err == 0);

    char *buf;
    size_t buf_len;
    err = js_get_arraybuffer_info(env, element, (void **) &buf, &buf_len);
    assert(err == 0);

    batch[i] = uv_buf_init(buf, buf_len);
  }

  err = udx_stream_write(req, stream, batch, len, on_udx_stream_ack);
  free(batch);

  if (err < 0) {
    js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, err, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_stream_write_sizeof (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  uint32_t bufs;
  err = js_get_value_uint32(env, argv[0], &bufs);
  assert(err == 0);

  js_value_t *return_uint32;
  err = js_create_uint32(env, udx_stream_write_sizeof(bufs), &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_stream_write_end (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[4];
  size_t argc = 4;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  udx_stream_write_t *req;
  size_t req_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &req, &req_len);
  assert(err == 0);

  uint32_t rid;
  err = js_get_value_uint32(env, argv[2], &rid);
  assert(err == 0);

  char *buf;
  size_t buf_len;
  err = js_get_arraybuffer_info(env, argv[3], (void **) &buf, &buf_len);
  assert(err == 0);

  req->data = (void *) ((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  err = udx_stream_write_end(req, stream, &b, 1, on_udx_stream_ack);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, err, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_stream_destroy (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_stream_t *stream;
  size_t stream_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &stream, &stream_len);
  assert(err == 0);

  err = udx_stream_destroy(stream);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  js_value_t *return_uint32;
  err = js_create_uint32(env, err, &return_uint32);
  assert(err == 0);

  return return_uint32;
}

js_value_t *
udx_napi_lookup (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[6];
  size_t argc = 6;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_napi_t *udx;
  size_t udx_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &udx, &udx_len);
  assert(err == 0);

  udx_napi_lookup_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &self, &self_len);
  assert(err == 0);

  self->udx = udx;

  size_t host_size = 0;
  err = js_get_value_string_utf8(env, argv[2], NULL, 0, &host_size);
  assert(err == 0);

  char *host = (char *) malloc((host_size + 1) * sizeof(char));
  size_t host_len;
  err = js_get_value_string_utf8(env, argv[2], (utf8_t *) host, host_size + 1, &host_len);
  assert(err == 0);
  host[host_size] = '\0';

  uint32_t family;
  err = js_get_value_uint32(env, argv[3], &family);
  assert(err == 0);

  udx_lookup_t *lookup = (udx_lookup_t *) self;

  self->host = host;
  self->env = env;
  err = js_create_reference(env, argv[4], 1, &(self->ctx));
  assert(err == 0);
  err = js_create_reference(env, argv[5], 1, &(self->on_lookup));
  assert(err == 0);

  int flags = 0;

  if (family == 4) flags |= UDX_LOOKUP_FAMILY_IPV4;
  if (family == 6) flags |= UDX_LOOKUP_FAMILY_IPV6;

  err = udx_lookup((udx_t *) udx, lookup, host, flags, on_udx_lookup);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  ensure_teardown(env, udx);

  return NULL;
}

js_value_t *
udx_napi_interface_event_init (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[5];
  size_t argc = 5;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_napi_t *udx;
  size_t udx_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &udx, &udx_len);
  assert(err == 0);

  udx_napi_interface_event_t *self;
  size_t self_len;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &self, &self_len);
  assert(err == 0);

  self->udx = udx;

  udx_interface_event_t *event = (udx_interface_event_t *) self;

  self->env = env;
  err = js_create_reference(env, argv[2], 1, &(self->ctx));
  assert(err == 0);
  err = js_create_reference(env, argv[3], 1, &(self->on_event));
  assert(err == 0);
  err = js_create_reference(env, argv[4], 1, &(self->on_close));
  assert(err == 0);

  err = udx_interface_event_init((udx_t *) udx, event, on_udx_interface_event_close);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  err = udx_interface_event_start(event, on_udx_interface_event, 5000);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  ensure_teardown(env, udx);

  return NULL;
}

js_value_t *
udx_napi_interface_event_start (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_interface_event_t *event;
  size_t event_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &event, &event_len);
  assert(err == 0);

  err = udx_interface_event_start(event, on_udx_interface_event, 5000);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_interface_event_stop (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_interface_event_t *event;
  size_t event_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &event, &event_len);
  assert(err == 0);

  err = udx_interface_event_stop(event);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_interface_event_close (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_interface_event_t *event;
  size_t event_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &event, &event_len);
  assert(err == 0);

  err = udx_interface_event_close(event);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return NULL;
  }

  return NULL;
}

js_value_t *
udx_napi_interface_event_get_addrs (js_env_t *env, js_callback_info_t *info) {
  int err;
  js_value_t *argv[1];
  size_t argc = 1;
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  udx_interface_event_t *event;
  size_t event_len;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &event, &event_len);
  assert(err == 0);

  char ip[INET6_ADDRSTRLEN];
  int family = 0;

  js_value_t *napi_result;
  err = js_create_array(env, &napi_result);
  assert(err == 0);

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

    js_value_t *napi_item;
    err = js_create_object(env, &napi_item);
    assert(err == 0);
    err = js_set_element(env, napi_result, j++, napi_item);
    assert(err == 0);

    js_value_t *napi_name;
    err = js_create_string_utf8(env, (utf8_t *) addr.name, NAPI_AUTO_LENGTH, &napi_name);
    assert(err == 0);
    err = js_set_named_property(env, napi_item, "name", napi_name);
    assert(err == 0);

    js_value_t *napi_ip;
    err = js_create_string_utf8(env, (utf8_t *) ip, NAPI_AUTO_LENGTH, &napi_ip);
    assert(err == 0);
    err = js_set_named_property(env, napi_item, "host", napi_ip);
    assert(err == 0);

    js_value_t *napi_family;
    err = js_create_uint32(env, family, &napi_family);
    assert(err == 0);
    err = js_set_named_property(env, napi_item, "family", napi_family);
    assert(err == 0);

    js_value_t *napi_internal;
    err = js_get_boolean(env, addr.is_internal, &napi_internal);
    assert(err == 0);
    err = js_set_named_property(env, napi_item, "internal", napi_internal);
    assert(err == 0);
  }

  return napi_result;
}

static js_value_t *
udx_native_exports (js_env_t *env, js_value_t *exports) {
  int err;

  // uint32
#define V(name, value) \
  { \
    js_value_t *val; \
    err = js_create_uint32(env, value, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("UV_UDP_IPV6ONLY", UV_UDP_IPV6ONLY);
  V("UV_UDP_REUSEADDR", UV_UDP_REUSEADDR);

  V("offsetof_udx_stream_t_inflight", offsetof(udx_stream_t, inflight));
  V("offsetof_udx_stream_t_mtu", offsetof(udx_stream_t, mtu));
  V("offsetof_udx_stream_t_cwnd", offsetof(udx_stream_t, cwnd));
  V("offsetof_udx_stream_t_srtt", offsetof(udx_stream_t, srtt));
  V("offsetof_udx_stream_t_bytes_rx", offsetof(udx_stream_t, bytes_rx));
  V("offsetof_udx_stream_t_packets_rx", offsetof(udx_stream_t, packets_rx));
  V("offsetof_udx_stream_t_bytes_tx", offsetof(udx_stream_t, bytes_tx));
  V("offsetof_udx_stream_t_packets_tx", offsetof(udx_stream_t, packets_tx));
  V("offsetof_udx_stream_t_rto_count", offsetof(udx_stream_t, rto_count));
  V("offsetof_udx_stream_t_retransmit_count", offsetof(udx_stream_t, retransmit_count));
  V("offsetof_udx_stream_t_fast_recovery_count", offsetof(udx_stream_t, fast_recovery_count));
  V("offsetof_udx_socket_t_bytes_rx", offsetof(udx_socket_t, bytes_rx));
  V("offsetof_udx_socket_t_packets_rx", offsetof(udx_socket_t, packets_rx));
  V("offsetof_udx_socket_t_bytes_tx", offsetof(udx_socket_t, bytes_tx));
  V("offsetof_udx_socket_t_packets_tx", offsetof(udx_socket_t, packets_tx));
  V("offsetof_udx_socket_t_packets_dropped_by_kernel", offsetof(udx_socket_t, packets_dropped_by_kernel));

  V("offsetof_udx_t_bytes_rx", offsetof(udx_t, bytes_rx));
  V("offsetof_udx_t_packets_rx", offsetof(udx_t, packets_rx));
  V("offsetof_udx_t_bytes_tx", offsetof(udx_t, bytes_tx));
  V("offsetof_udx_t_packets_tx", offsetof(udx_t, packets_tx));
  V("offsetof_udx_t_packets_dropped_by_kernel", offsetof(udx_t, packets_dropped_by_kernel));

  V("sizeof_udx_napi_t", sizeof(udx_napi_t));
  V("sizeof_udx_napi_socket_t", sizeof(udx_napi_socket_t));
  V("sizeof_udx_napi_stream_t", sizeof(udx_napi_stream_t));
  V("sizeof_udx_napi_lookup_t",  sizeof(udx_napi_lookup_t));
  V("sizeof_udx_napi_interface_event_t", sizeof(udx_napi_interface_event_t));
  V("sizeof_udx_socket_send_t", sizeof(udx_socket_send_t));
  V("sizeof_udx_stream_send_t", sizeof(udx_stream_send_t));
#undef V

  // functions
#define V(name, untyped, signature, typed) \
  { \
    js_value_t *val; \
    if (signature) { \
      err = js_create_typed_function(env, name, -1, untyped, signature, typed, NULL, &val); \
      assert(err == 0); \
    } else { \
      err = js_create_function(env, name, -1, untyped, NULL, &val); \
      assert(err == 0); \
    } \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("udx_napi_init", udx_napi_init, NULL, NULL);
  V("udx_napi_socket_init", udx_napi_socket_init, NULL, NULL);
  V("udx_napi_socket_bind", udx_napi_socket_bind, NULL, NULL);
  V("udx_napi_socket_set_ttl", udx_napi_socket_set_ttl, NULL, NULL);
  V("udx_napi_socket_get_recv_buffer_size", udx_napi_socket_get_recv_buffer_size, NULL, NULL);
  V("udx_napi_socket_set_recv_buffer_size", udx_napi_socket_set_recv_buffer_size, NULL, NULL);
  V("udx_napi_socket_get_send_buffer_size", udx_napi_socket_get_send_buffer_size, NULL, NULL);
  V("udx_napi_socket_set_send_buffer_size", udx_napi_socket_set_send_buffer_size, NULL, NULL);
  V("udx_napi_socket_set_membership", udx_napi_socket_set_membership, NULL, NULL);
  V("udx_napi_socket_send_ttl", udx_napi_socket_send_ttl, NULL, NULL);
  V("udx_napi_socket_close", udx_napi_socket_close, NULL, NULL);
  V("udx_napi_stream_init", udx_napi_stream_init, NULL, NULL);
  V("udx_napi_stream_set_seq", udx_napi_stream_set_seq, NULL, NULL);
  V("udx_napi_stream_set_ack", udx_napi_stream_set_ack, NULL, NULL);
  V("udx_napi_stream_set_mode", udx_napi_stream_set_mode, NULL, NULL);
  V("udx_napi_stream_connect", udx_napi_stream_connect, NULL, NULL);
  V("udx_napi_stream_change_remote", udx_napi_stream_change_remote, NULL, NULL);
  V("udx_napi_stream_relay_to", udx_napi_stream_relay_to, NULL, NULL);
  V("udx_napi_stream_send", udx_napi_stream_send, NULL, NULL);
  V("udx_napi_stream_recv_start", udx_napi_stream_recv_start, NULL, NULL);
  V("udx_napi_stream_write", udx_napi_stream_write, NULL, NULL);
  V("udx_napi_stream_writev", udx_napi_stream_writev, NULL, NULL);
  V("udx_napi_stream_write_sizeof", udx_napi_stream_write_sizeof, NULL, NULL);
  V("udx_napi_stream_write_end", udx_napi_stream_write_end, NULL, NULL);
  V("udx_napi_stream_destroy", udx_napi_stream_destroy, NULL, NULL);
  V("udx_napi_lookup", udx_napi_lookup, NULL, NULL);
  V("udx_napi_interface_event_init", udx_napi_interface_event_init, NULL, NULL);
  V("udx_napi_interface_event_start", udx_napi_interface_event_start, NULL, NULL);
  V("udx_napi_interface_event_stop", udx_napi_interface_event_stop, NULL, NULL);
  V("udx_napi_interface_event_close", udx_napi_interface_event_close, NULL, NULL);
  V("udx_napi_interface_event_get_addrs", udx_napi_interface_event_get_addrs, NULL, NULL);
#undef V

  return exports;
}

BARE_MODULE(udx_native, udx_native_exports)
