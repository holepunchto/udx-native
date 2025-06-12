#include <bare.h>
#include <js.h>
#include <jstl.h>
#include <stdlib.h>
#include <string.h>
#include <udx.h>

#define UDX_NAPI_INTERACTIVE     0
#define UDX_NAPI_NON_INTERACTIVE 1
#define UDX_NAPI_FRAMED          2

namespace {
// socket
using cb_socket_send_t = js_function_t<void, js_receiver_t, uint64_t, int>;
using cb_socket_message_t = js_function_t<js_typedarray_span_t<>, js_receiver_t, int64_t, int, std::string, int>;
using cb_socket_close_t = js_function_t<void, js_receiver_t>;
using cb_socket_realloc_message_t = js_function_t<js_typedarray_span_t<>, js_receiver_t>;

// stream
using cb_stream_end_t = js_function_t<void, js_receiver_t, uint32_t>;
using cb_stream_data_t = js_function_t<js_typedarray_span_t<>, js_receiver_t, uint32_t>;
using cb_stream_drain_t = js_function_t<void, js_receiver_t>;
using cb_stream_ack_t = js_function_t<void, js_receiver_t, uint32_t>;
using cb_stream_send_t = js_function_t<void, js_receiver_t, int32_t, int32_t>;
using cb_stream_message_t = js_function_t<js_typedarray_span_t<>, js_receiver_t, uint32_t>;
using cb_stream_realloc_data_t = js_function_t<js_typedarray_span_t<>, js_receiver_t>;
using cb_stream_realloc_message_t = js_function_t<js_typedarray_span_t<>, js_receiver_t>;
using cb_stream_close_t = js_function_t<void, js_receiver_t, std::optional<js_object_t>>;
using cb_stream_firewall_t = js_function_t<uint32_t, js_receiver_t, js_receiver_t, uint32_t, std::string, uint32_t>;
using cb_stream_remote_changed_t = js_function_t<void, js_receiver_t>;

// udx
using cb_udx_lookup_t = js_function_t<void, js_receiver_t, std::optional<js_object_t>, std::optional<std::string>, int32_t>;

// interface
using cb_interface_event_t = js_function_t<void, js_receiver_t>;
using cb_interface_close_t = js_function_t<void, js_receiver_t>;
}; // namespace

struct udx_napi_t {
  udx_t udx;

  uint8_t *read_buf;
  size_t read_buf_free;

  js_deferred_teardown_t *teardown;
  bool exiting;
};

struct udx_napi_socket_t {
  udx_socket_t socket;
  udx_napi_t *udx;

  js_env_t *env;

  js_persistent_t<js_receiver_t> ctx;

  js_persistent_t<cb_socket_send_t> on_send;
  js_persistent_t<cb_socket_message_t> on_message;
  js_persistent_t<cb_socket_close_t> on_close;
  js_persistent_t<cb_socket_realloc_message_t> realloc_message; // TODO: deprecate
};

struct udx_napi_stream_t {
  udx_stream_t stream;
  udx_napi_t *udx;

  int mode;

  uint8_t *read_buf;
  uint8_t *read_buf_head;
  size_t read_buf_free;

  ssize_t frame_len;

  js_env_t *env;
  js_persistent_t<js_receiver_t> ctx;
  js_persistent_t<cb_stream_data_t> on_data;
  js_persistent_t<cb_stream_end_t> on_end;
  js_persistent_t<cb_stream_drain_t> on_drain;
  js_persistent_t<cb_stream_ack_t> on_ack;
  js_persistent_t<cb_stream_send_t> on_send;
  js_persistent_t<cb_stream_message_t> on_message;
  js_persistent_t<cb_stream_close_t> on_close;
  js_persistent_t<cb_stream_firewall_t> on_firewall;
  js_persistent_t<cb_stream_remote_changed_t> on_remote_changed;
  js_persistent_t<cb_stream_realloc_data_t> realloc_data;
  js_persistent_t<cb_stream_realloc_message_t> realloc_message; // TODO: deprecate
};

struct udx_napi_lookup_t {
  udx_lookup_t handle;
  udx_napi_t *udx;

  js_env_t *env;
  js_persistent_t<js_receiver_t> ctx;
  js_persistent_t<cb_udx_lookup_t> on_lookup;
};

struct udx_napi_interface_event_t {
  udx_interface_event_t handle;
  udx_napi_t *udx;

  js_env_t *env;
  js_persistent_t<js_receiver_t> ctx;
  js_persistent_t<cb_interface_event_t> on_event;
  js_persistent_t<cb_interface_close_t> on_close;
};

inline static void
parse_address (const struct sockaddr *name, char *ip, size_t size, int *port, int *family) {
  if (name->sa_family == AF_INET) {
    auto name_v4 = reinterpret_cast<const struct sockaddr_in *>(name);

    if (port) *port = ntohs(name_v4->sin_port);
    if (family) *family = 4;

    uv_ip4_name(name_v4, ip, size);
  } else if (name->sa_family == AF_INET6) {
    auto name_v6 = reinterpret_cast<const struct sockaddr_in6 *>(name);

    if (port) *port = ntohs(name_v6->sin6_port);
    if (family) *family = 6;

    uv_ip6_name(name_v6, ip, size);
  }
}

inline static int
load_address (js_env_t *env, char *addr, std::string str) {
  memset(addr, 0, INET6_ADDRSTRLEN);

  const char *val = str.c_str();

  size_t len = INET6_ADDRSTRLEN;
  if (str.length() < INET6_ADDRSTRLEN) len = str.length();

  strncpy(addr, val, len);

  return len;
}

static void
on_udx_send (udx_socket_send_t *req, int status) {
  int err;

  auto *n = reinterpret_cast<udx_napi_socket_t *>(req->socket);

  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_socket_send_t callback;
  err = js_get_reference_value(env, n->on_send, callback);
  assert(err == 0);

  auto id = reinterpret_cast<uintptr_t>(req->data);

  err = js_call_function_with_checkpoint(env, callback, ctx, uint64_t(id), status);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_message (udx_socket_t *self, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  auto *n = reinterpret_cast<udx_napi_socket_t *>(self);

  if (n->udx->exiting) return;

  int err;
  int port = 0;
  char ip[INET6_ADDRSTRLEN];
  int family = 0;
  parse_address(from, ip, INET6_ADDRSTRLEN, &port, &family);

  if (buf->len > n->udx->read_buf_free) return;

  memcpy(n->udx->read_buf, buf->base, buf->len);

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_socket_message_t callback;
  err = js_get_reference_value(env, n->on_message, callback);
  assert(err == 0);

  std::string ip_str;
  ip_str = ip;

  js_typedarray_span_t<> res;
  err = js_call_function_with_checkpoint<js_type_options_t{}, js_typedarray_span_t<>, js_receiver_t, int64_t, int, std::string, int>(
    env,
    callback,
    ctx,
    int64_t(read_len),
    port,
    ip_str,
    family,
    res
  );

  if (err == 0) {
    n->udx->read_buf = res.data();
    n->udx->read_buf_free = res.size();
  } else {
    // avoid reentry
    if (!(n->udx->exiting)) {
      int err;
      js_env_t *env = n->env;

      js_handle_scope_t *scope;
      err = js_open_handle_scope(env, &scope);
      assert(err == 0);

      js_receiver_t ctx;
      err = js_get_reference_value(env, n->ctx, ctx);
      assert(err == 0);

      cb_socket_realloc_message_t callback;
      err = js_get_reference_value(env, n->realloc_message, callback);
      assert(err == 0);

      err = js_call_function_with_checkpoint<js_type_options_t{}, js_typedarray_span_t<>, js_receiver_t>(env, callback, ctx, res);
      assert(err == 0);

      n->udx->read_buf = res.data();
      n->udx->read_buf_free = res.size();

      err = js_close_handle_scope(env, scope);
      assert(err == 0);
    }
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_close (udx_socket_t *self) {
  auto n = reinterpret_cast<udx_napi_socket_t *>(self);
  int err;
  js_env_t *env = n->env;

  if (!(n->udx->exiting)) {
    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    js_receiver_t ctx;
    err = js_get_reference_value(env, n->ctx, ctx);
    assert(err == 0);

    cb_socket_close_t callback;
    err = js_get_reference_value(env, n->on_close, callback);
    assert(err == 0);

    err = js_call_function_with_checkpoint(env, callback, ctx);
    assert(err == 0);

    err = js_close_handle_scope(env, scope);
    assert(err == 0);
  }

  n->on_send.reset();
  n->on_message.reset();
  n->on_close.reset();
  n->realloc_message.reset();
  n->ctx.reset();
}

static void
on_udx_teardown (js_deferred_teardown_t *handle, void *data) {
  auto self = reinterpret_cast<udx_napi_t *>(data);

  self->exiting = true;

  udx_teardown(&self->udx);
}

static void
ensure_teardown (js_env_t *env, udx_napi_t *udx) {
  if (udx->teardown) return;

  int err = js_add_deferred_teardown_callback(
    env,
    on_udx_teardown,
    reinterpret_cast<void *>(udx),
    &udx->teardown
  );

  if (err != 0) abort();
}

static void
on_udx_stream_end (udx_stream_t *stream) {
  int err;
  auto n = reinterpret_cast<udx_napi_stream_t *>(stream);
  if (n->udx->exiting) return;

  size_t read = n->read_buf_head - n->read_buf;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_end_t callback;
  err = js_get_reference_value(env, n->on_end, callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, callback, ctx, uint32_t(read));
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len == UV_EOF) return on_udx_stream_end(stream);

  auto n = reinterpret_cast<udx_napi_stream_t *>(stream);
  if (n->udx->exiting) return;

  // ignore the message if it doesn't fit in the read buffer
  if (buf->len > n->read_buf_free) return;

  if (n->mode == UDX_NAPI_FRAMED && n->frame_len == -1) {
    if (buf->len < 3) {
      n->mode = UDX_NAPI_INTERACTIVE;
    } else {
      auto b = reinterpret_cast<uint8_t *>(buf->base);
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

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_data_t callback;
  err = js_get_reference_value(env, n->on_data, callback);
  assert(err == 0);

  js_typedarray_span_t<> res;
  err = js_call_function_with_checkpoint<js_type_options_t{}, js_typedarray_span_t<>, js_receiver_t, uint32_t>(env, callback, ctx, uint32_t(read), res);

  if (err == 0) {
    n->read_buf = res.data();
    n->read_buf_free = res.size();
    n->read_buf_head = n->read_buf;
  } else {
    // avoid re-entry
    if (!(n->udx->exiting)) {
      js_handle_scope_t *inner_scope;
      err = js_open_handle_scope(env, &inner_scope);
      assert(err == 0);

      js_receiver_t ctx;
      err = js_get_reference_value(env, n->ctx, ctx);
      assert(err == 0);

      cb_stream_realloc_data_t callback;
      err = js_get_reference_value(env, n->realloc_data, callback);
      assert(err == 0);

      err = js_call_function_with_checkpoint<js_type_options_t{}, js_typedarray_span_t<>, js_receiver_t>(env, callback, ctx, res);
      assert(err == 0);

      n->read_buf = res.data();
      n->read_buf_free = res.size();
      n->read_buf_head = n->read_buf;

      err = js_close_handle_scope(env, inner_scope);
      assert(err == 0);
    }
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_drain (udx_stream_t *stream) {
  auto n = reinterpret_cast<udx_napi_stream_t *>(stream);
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  int err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_drain_t callback;
  err = js_get_reference_value(env, n->on_drain, callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, callback, ctx);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_ack (udx_stream_write_t *req, int status, int unordered) {
  int err = 0;

  auto n = reinterpret_cast<udx_napi_stream_t *>(req->stream);
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_ack_t callback;
  err = js_get_reference_value(env, n->on_ack, callback);
  assert(err == 0);

  auto offset = reinterpret_cast<uintptr_t>(req->data);

  err = js_call_function_with_checkpoint(env, callback, ctx, uint32_t(offset));
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_send (udx_stream_send_t *req, int status) {
  int err;

  auto n = reinterpret_cast<udx_napi_stream_t *>(req->stream);

  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_send_t callback;
  err = js_get_reference_value(env, n->on_send, callback);
  assert(err == 0);

  auto id = reinterpret_cast<uintptr_t>(req->data);

  err = js_call_function_with_checkpoint(env, callback, ctx, int32_t(id), status);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_recv (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  int err;

  auto n = reinterpret_cast<udx_napi_stream_t *>(stream);

  if (n->udx->exiting) return;

  if (buf->len > n->udx->read_buf_free) return;

  memcpy(n->udx->read_buf, buf->base, buf->len);

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_message_t callback;
  err = js_get_reference_value(env, n->on_message, callback);
  assert(err == 0);

  js_typedarray_span_t<> res;
  err = js_call_function_with_checkpoint<js_type_options_t{}, js_typedarray_span_t<>, js_receiver_t, uint32_t>(env, callback, ctx, uint32_t(read_len), res);

  if (err == 0) {
    n->udx->read_buf = res.data();
    n->udx->read_buf_free = res.size();
  } else {
    // avoid re-entry
    if (!(n->udx->exiting)) {
      js_handle_scope_t *inner_scope;
      err = js_open_handle_scope(env, &inner_scope);
      assert(err == 0);

      js_receiver_t ctx;
      err = js_get_reference_value(env, n->ctx, ctx);
      assert(err == 0);

      cb_stream_realloc_message_t callback;
      err = js_get_reference_value(env, n->realloc_message, callback);
      assert(err == 0);

      err = js_call_function_with_checkpoint<js_type_options_t{}, js_typedarray_span_t<>, js_receiver_t>(env, callback, ctx, res);
      assert(err == 0);

      n->udx->read_buf = res.data();
      n->udx->read_buf_free = res.size();

      err = js_close_handle_scope(env, inner_scope);
      assert(err == 0);
    }
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_stream_finalize (udx_stream_t *stream) {
  auto n = reinterpret_cast<udx_napi_stream_t *>(stream);

  n->on_data.reset();
  n->on_end.reset();
  n->on_drain.reset();
  n->on_ack.reset();
  n->on_send.reset();
  n->on_message.reset();
  n->on_close.reset();
  n->on_firewall.reset();
  n->on_remote_changed.reset();
  n->realloc_data.reset();
  n->realloc_message.reset();
  n->ctx.reset();
}

static void
on_udx_stream_close (udx_stream_t *stream, int status) {
  int err;

  auto n = reinterpret_cast<udx_napi_stream_t *>(stream);

  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_close_t callback;
  err = js_get_reference_value(env, n->on_close, callback);
  assert(err == 0);

  std::optional<js_object_t> res;

  if (status < 0) {
    js_string_t code;
    err = js_create_string(env, uv_err_name(status), code);
    assert(err == 0);

    res.emplace(); // init

    err = js_create_error(env, code, uv_strerror(status), *res);
    assert(err == 0);
  }

  err = js_call_function_with_checkpoint(env, callback, ctx, res);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static int
on_udx_stream_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  int err;
  auto *n = reinterpret_cast<udx_napi_stream_t *>(stream);
  auto *s = reinterpret_cast<udx_napi_socket_t *>(socket);

  uint32_t fw = 1; // assume error means firewall it, whilst reporting the uncaught
  if (n->udx->exiting) return fw;

  int port = 0;
  char ip[INET6_ADDRSTRLEN];
  int family = 0;
  parse_address(from, ip, INET6_ADDRSTRLEN, &port, &family);

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_firewall_t callback;
  err = js_get_reference_value(env, n->on_firewall, callback);
  assert(err == 0);

  js_receiver_t socket_ctx;
  err = js_get_reference_value(env, s->ctx, socket_ctx);
  assert(err == 0);

  err = js_call_function_with_checkpoint<js_type_options_t{}, uint32_t, js_receiver_t, js_receiver_t, uint32_t, std::string, uint32_t>(
    env,
    callback,
    ctx,
    socket_ctx,
    uint32_t(port),
    std::string(ip),
    uint32_t(family),
    fw
  );
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  return fw;
}

static void
on_udx_stream_remote_changed (udx_stream_t *stream) {
  int err;

  auto n = reinterpret_cast<udx_napi_stream_t *>(stream);

  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_stream_remote_changed_t callback;
  err = js_get_reference_value(env, n->on_remote_changed, callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, callback, ctx);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_lookup (udx_lookup_t *lookup, int status, const struct sockaddr *addr, int addr_len) {
  int err;

  auto n = reinterpret_cast<udx_napi_lookup_t *>(lookup);
  if (n->udx->exiting) return;

  js_env_t *env = n->env;

  char ip[INET6_ADDRSTRLEN] = "";
  int family = 0;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, n->ctx, ctx);
  assert(err == 0);

  cb_udx_lookup_t callback;
  err = js_get_reference_value(env, n->on_lookup, callback);
  assert(err == 0);

  std::optional<js_object_t> error;
  std::optional<std::string> ip_str;

  if (status >= 0) {
    parse_address(addr, ip, addr_len, nullptr, &family);

    ip_str = ip;
  } else {
    js_string_t code;
    err = js_create_string(env, uv_err_name(status), code);
    assert(err == 0);

    error.emplace(); // init

    err = js_create_error(env, code, uv_strerror(status), *error);
    assert(err == 0);
  }

  err = js_call_function_with_checkpoint(env, callback, ctx, error, ip_str, family);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  n->on_lookup.reset();
  n->ctx.reset();
}

static void
on_udx_interface_event (udx_interface_event_t *handle, int status) {
  auto e = reinterpret_cast<udx_napi_interface_event_t *>(handle);
  if (e->udx->exiting) return;

  js_env_t *env = e->env;

  js_handle_scope_t *scope;
  int err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, e->ctx, ctx);
  assert(err == 0);

  cb_interface_event_t callback;
  err = js_get_reference_value(env, e->on_event, callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, callback, ctx);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_udx_interface_event_close (udx_interface_event_t *handle) {
  auto e = reinterpret_cast<udx_napi_interface_event_t *>(handle);

  if (e->udx->exiting) return;

  js_env_t *env = e->env;

  js_handle_scope_t *scope;
  int err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_receiver_t ctx;
  err = js_get_reference_value(env, e->ctx, ctx);
  assert(err == 0);

  cb_interface_close_t callback;
  err = js_get_reference_value(env, e->on_close, callback);
  assert(err == 0);

  err = js_call_function_with_checkpoint(env, callback, ctx);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  e->on_event.reset();
  e->on_close.reset();
  e->ctx.reset();
}

static void
on_udx_idle (udx_t *u) {
  auto self = reinterpret_cast<udx_napi_t *>(u);

  if (!self->teardown) return;

  int err = js_finish_deferred_teardown_callback(self->teardown);

  self->teardown = nullptr;

  if (err != 0) abort();
}

static inline void
udx_napi_init (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_t, 1> self,
  js_typedarray_span_t<> read_buf
) {
  int err;

  *self = {};

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  err = udx_init(loop, &self->udx, on_udx_idle);
  assert(err == 0);

  self->read_buf = read_buf.data();
  self->read_buf_free = read_buf.size();

  self->exiting = false;
  self->teardown = nullptr;
}

static inline void
udx_napi_socket_init (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_t, 1> udx,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self,

  js_receiver_t ctx,

  cb_socket_send_t on_send,
  cb_socket_message_t on_message,
  cb_socket_close_t on_close,
  cb_socket_realloc_message_t realloc_message
) {
  int err;

  *self = {};

  udx_socket_t *socket = &self->socket;
  self->udx = &*udx;
  self->env = env;

  err = udx_socket_init(&udx->udx, socket, on_udx_close);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);

    return;
  }

  err = js_create_reference(env, ctx, self->ctx);
  assert(err == 0);
  err = js_create_reference(env, on_send, self->on_send);
  assert(err == 0);
  err = js_create_reference(env, on_message, self->on_message);
  assert(err == 0);
  err = js_create_reference(env, on_close, self->on_close);
  assert(err == 0);
  err = js_create_reference(env, realloc_message, self->realloc_message);
  assert(err == 0);

  ensure_teardown(env, udx);
}

static inline int32_t
udx_napi_socket_bind (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self,
  uint32_t port,
  std::string ip_str,
  uint32_t family,
  uint32_t flags
) {
  int err;

  auto socket = &self->socket;

  char ip[INET6_ADDRSTRLEN];
  load_address(env, ip, ip_str);

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

    return -1;
  }

  err = udx_socket_bind(socket, (struct sockaddr *) &addr, flags);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return -1;
  }

  // TODO: move the bottom stuff into another function, start, so error handling is easier

  struct sockaddr_storage name;

  // wont error in practice
  err = udx_socket_getsockname(socket, (struct sockaddr *) &name, &addr_len);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return -1;
  }

  int local_port;

  if (family == 4) {
    local_port = ntohs(((struct sockaddr_in *) &name)->sin_port);
  } else {
    local_port = ntohs(((struct sockaddr_in6 *) &name)->sin6_port);
  }

  // wont error in practice
  err = udx_socket_recv_start(socket, on_udx_message);
  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return -1;
  }

  return local_port;
}

static inline void
udx_napi_socket_set_ttl (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self,
  uint32_t ttl
) {
  int err;

  err = udx_socket_set_ttl(&self->socket, ttl);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline void
udx_napi_socket_set_membership (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> socket,
  std::string mcast_addr_str,
  std::string iface_addr_str,
  bool join
) {
  int err;

  char mcast_addr[INET6_ADDRSTRLEN];
  load_address(env, mcast_addr, mcast_addr_str);

  char iface_addr[INET6_ADDRSTRLEN];
  size_t iface_addr_len = load_address(env, iface_addr, iface_addr_str);

  char *iface_param = iface_addr_len > 0 ? iface_addr : NULL;

  err = udx_socket_set_membership(&socket->socket, mcast_addr, iface_param, join ? UV_JOIN_GROUP : UV_LEAVE_GROUP);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline uint32_t
udx_napi_socket_get_recv_buffer_size (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self
) {
  int err;

  int size = 0;

  err = udx_socket_get_recv_buffer_size(&self->socket, &size);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }

  return size;
}

static inline void
udx_napi_socket_set_recv_buffer_size (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self,
  uint32_t size
) {
  int err = udx_socket_set_recv_buffer_size(&self->socket, size);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline uint32_t
udx_napi_socket_get_send_buffer_size (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self
) {
  int size = 0;

  int err = udx_socket_get_send_buffer_size(&self->socket, &size);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }

  return size;
}

static inline uint32_t
udx_napi_socket_set_send_buffer_size (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self,
  uint32_t size
) {
  int err = udx_socket_set_send_buffer_size(&self->socket, size);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }

  return size;
}

static inline void
udx_napi_socket_send_ttl (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self,
  js_typedarray_span_of_t<udx_socket_send_t, 1> req,
  uint32_t rid,
  js_typedarray_span_of_t<char> buf,
  uint32_t port,
  std::string ip_str,
  uint32_t family,
  uint32_t ttl
) {
  int err;

  char ip[INET6_ADDRSTRLEN];
  load_address(env, ip, ip_str);

  req->data = reinterpret_cast<void *>(static_cast<uintptr_t>(rid));

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return;
  }

  uv_buf_t b = uv_buf_init(buf.data(), buf.size());

  err = udx_socket_send_ttl(req, &self->socket, &b, 1, (const struct sockaddr *) &addr, ttl, on_udx_send);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline void
udx_napi_socket_close (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> self
) {
  int err;

  err = udx_socket_close(&self->socket);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline void
udx_napi_stream_init (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_t, 1> udx,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> self,

  uint32_t id,
  uint32_t framed,
  js_receiver_t ctx,

  cb_stream_data_t on_data,
  cb_stream_end_t on_end,
  cb_stream_drain_t on_drain,
  cb_stream_ack_t on_ack,
  cb_stream_send_t on_send,
  cb_stream_message_t on_message,
  cb_stream_close_t on_close,
  cb_stream_firewall_t on_firewall,
  cb_stream_remote_changed_t on_remote_changed,
  cb_stream_realloc_data_t realloc_data,
  cb_stream_realloc_message_t realloc_message
) {
  int err;

  *self = {};

  self->mode = framed ? UDX_NAPI_FRAMED : UDX_NAPI_INTERACTIVE;

  self->frame_len = -1;

  self->read_buf = NULL;
  self->read_buf_head = NULL;
  self->read_buf_free = 0;

  self->udx = udx;
  self->env = env;

  auto stream = &self->stream;

  err = udx_stream_init(&udx->udx, stream, id, on_udx_stream_close, on_udx_stream_finalize);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);

    return;
  }

  err = js_create_reference(env, ctx, self->ctx);
  assert(err == 0);
  err = js_create_reference(env, on_data, self->on_data);
  assert(err == 0);
  err = js_create_reference(env, on_end, self->on_end);
  assert(err == 0);
  err = js_create_reference(env, on_drain, self->on_drain);
  assert(err == 0);
  err = js_create_reference(env, on_ack, self->on_ack);
  assert(err == 0);
  err = js_create_reference(env, on_send, self->on_send);
  assert(err == 0);
  err = js_create_reference(env, on_message, self->on_message);
  assert(err == 0);
  err = js_create_reference(env, on_close, self->on_close);
  assert(err == 0);
  err = js_create_reference(env, on_firewall, self->on_firewall);
  assert(err == 0);
  err = js_create_reference(env, on_remote_changed, self->on_remote_changed);
  assert(err == 0);
  err = js_create_reference(env, realloc_data, self->realloc_data);
  assert(err == 0);
  err = js_create_reference(env, realloc_message, self->realloc_message);
  assert(err == 0);

  udx_stream_firewall(stream, on_udx_stream_firewall);
  udx_stream_write_resume(stream, on_udx_stream_drain);

  ensure_teardown(env, udx);
}

static inline void
udx_napi_stream_set_seq (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  uint32_t seq
) {
  int err = udx_stream_set_seq(&stream->stream, seq);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline void
udx_napi_stream_set_ack (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  uint32_t ack
) {
  int err = udx_stream_set_ack(&stream->stream, ack);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline void
udx_napi_stream_set_mode (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  uint32_t mode
) {
  stream->mode = mode;
}

static inline void
udx_napi_stream_recv_start (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  js_typedarray_span_t<> read_buf
) {
  int err;

  stream->read_buf = read_buf.data();
  stream->read_buf_head = read_buf.data();
  stream->read_buf_free = read_buf.size();

  err = udx_stream_read_start(&stream->stream, on_udx_stream_read);
  assert(err == 0);
  err = udx_stream_recv_start(&stream->stream, on_udx_stream_recv);
  assert(err == 0);
}

static inline void
udx_napi_stream_connect (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> socket,
  uint32_t remote_id,
  uint32_t port,
  std::string ip_str,
  uint32_t family
) {
  int err;

  char ip[INET6_ADDRSTRLEN];
  load_address(env, ip, ip_str);

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return;
  }

  err = udx_stream_connect(&stream->stream, &socket->socket, remote_id, (const struct sockaddr *) &addr);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline void
udx_napi_stream_change_remote (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  js_typedarray_span_of_t<udx_napi_socket_t, 1> socket,
  uint32_t remote_id,
  uint32_t port,
  std::string ip_str,
  uint32_t family
) {
  int err;

  char ip[INET6_ADDRSTRLEN];
  load_address(env, ip, ip_str);

  struct sockaddr_storage addr;

  if (family == 4) {
    err = uv_ip4_addr(ip, port, (struct sockaddr_in *) &addr);
  } else {
    err = uv_ip6_addr(ip, port, (struct sockaddr_in6 *) &addr);
  }

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return;
  }

  int immediate = err = udx_stream_change_remote(&stream->stream, &socket->socket, remote_id, (const struct sockaddr *) &addr, on_udx_stream_remote_changed);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
    return;
  }

  if (immediate == 1) {
    js_receiver_t ctx;
    err = js_get_reference_value(env, stream->ctx, ctx);
    assert(err == 0);

    cb_stream_remote_changed_t callback;
    err = js_get_reference_value(env, stream->on_remote_changed, callback);
    assert(err == 0);

    err = js_call_function(env, callback, ctx);
    assert(err == 0);
  }
}

static inline void
udx_napi_stream_relay_to (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> destination
) {
  int err = udx_stream_relay_to(&stream->stream, &destination->stream);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline uint32_t
udx_napi_stream_send (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  js_typedarray_span_of_t<udx_stream_send_t, 1> req,
  uint32_t rid,
  js_typedarray_span_of_t<char> buf
) {
  req->data = reinterpret_cast<void *>(static_cast<uintptr_t>(rid));

  uv_buf_t b = uv_buf_init(buf.data(), buf.size());

  int res = udx_stream_send(req, &stream->stream, &b, 1, on_udx_stream_send);

  if (res < 0) {
    int err = js_throw_error(env, uv_err_name(res), uv_strerror(res));
    assert(err == 0);
  }

  return res;
}

static inline int64_t
udx_napi_stream_write (
  js_env_t *env,
  js_receiver_t,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  js_typedarray_span_t<> req_handle,
  uint32_t rid,
  js_typedarray_span_of_t<char> buf
) {
  int err;

  auto req = reinterpret_cast<udx_stream_write_t *>(req_handle.data());

  req->data = reinterpret_cast<void *>(static_cast<uintptr_t>(rid));

  auto b = uv_buf_init(buf.data(), buf.size());

  int res = udx_stream_write(&*req, &stream->stream, &b, 1, on_udx_stream_ack);

  if (res < 0) {
    err = js_throw_error(env, uv_err_name(res), uv_strerror(res));
    assert(err == 0);
  }

  return res;
}

// TODO: use only on platforms without fastcall support
static inline int64_t
udx_napi_stream_writev (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  js_typedarray_span_t<> req_handle,
  uint32_t rid,
  std::vector<js_typedarray_span_of_t<char>> buffers
) {
  int err;

  auto req = reinterpret_cast<udx_stream_write_t *>(req_handle.data());

  req->data = reinterpret_cast<void *>(static_cast<uintptr_t>(rid));

  const auto len = buffers.size();

  std::vector<uv_buf_t> batch(len);

  for (int i = 0; i < len; i++) {
    auto buf = buffers[i];
    batch[i] = uv_buf_init(buf.data(), buf.size());
  }

  int res = udx_stream_write(req, &stream->stream, batch.data(), len, on_udx_stream_ack);

  if (res < 0) {
    err = js_throw_error(env, uv_err_name(res), uv_strerror(res));
    assert(err == 0);
  }

  return res;
}

static inline uint32_t
udx_napi_stream_write_sizeof (uint32_t bufs) {
  return udx_stream_write_sizeof(bufs);
}

static inline uint32_t
udx_napi_stream_write_end (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream,
  js_typedarray_span_t<> req_handle,
  uint32_t rid,
  js_typedarray_span_of_t<char> buf
) {

  auto req = reinterpret_cast<udx_stream_write_t *>(req_handle.data());

  req->data = reinterpret_cast<void *>(static_cast<uintptr_t>(rid));

  uv_buf_t b = uv_buf_init(buf.data(), buf.size());

  int res = udx_stream_write_end(req, &stream->stream, &b, 1, on_udx_stream_ack);

  if (res < 0) {
    int err = js_throw_error(env, uv_err_name(res), uv_strerror(res));
    assert(err == 0);
  }

  return res;
}

static inline uint32_t
udx_napi_stream_destroy (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_stream_t, 1> stream
) {
  int res = udx_stream_destroy(&stream->stream);

  if (res < 0) {
    int err = js_throw_error(env, uv_err_name(res), uv_strerror(res));
    assert(err == 0);
  }

  return res;
}

static inline void
udx_napi_lookup (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_t, 1> udx,
  js_typedarray_span_of_t<udx_napi_lookup_t, 1> self,
  js_string_t host_str,
  uint32_t family,
  js_receiver_t ctx, // TODO: use auto receiver
  cb_udx_lookup_t on_lookup
) {
  int err;

  *self = {};

  self->udx = udx;

  std::string host;
  err = js_get_value_string(env, host_str, host);
  assert(err == 0);

  udx_lookup_t *lookup = &self->handle;

  self->env = env;

  int flags = 0;

  if (family == 4) flags |= UDX_LOOKUP_FAMILY_IPV4;
  if (family == 6) flags |= UDX_LOOKUP_FAMILY_IPV6;

  err = udx_lookup(&udx->udx, lookup, host.c_str(), flags, on_udx_lookup);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);

    return;
  }

  err = js_create_reference(env, ctx, self->ctx);
  assert(err == 0);
  err = js_create_reference(env, on_lookup, self->on_lookup);
  assert(err == 0);

  ensure_teardown(env, udx);
}

static inline void
udx_napi_interface_event_init (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_t, 1> udx,
  js_typedarray_span_of_t<udx_napi_interface_event_t, 1> self,
  js_receiver_t ctx,
  cb_interface_event_t on_event,
  cb_interface_close_t on_close
) {
  int err;

  *self = {};

  self->udx = udx;
  self->env = env;

  err = udx_interface_event_init(&udx->udx, &self->handle, on_udx_interface_event_close);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);

    return;
  }

  err = udx_interface_event_start(&self->handle, on_udx_interface_event, 5000);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);

    return;
  }

  err = js_create_reference(env, ctx, self->ctx);
  assert(err == 0);
  err = js_create_reference(env, on_event, self->on_event);
  assert(err == 0);
  err = js_create_reference(env, on_close, self->on_close);
  assert(err == 0);

  ensure_teardown(env, udx);
}

static inline void
udx_napi_interface_event_start (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_interface_event_t, 1> self
) {
  int err = udx_interface_event_start(&self->handle, on_udx_interface_event, 5000);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline void
udx_napi_interface_event_stop (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_interface_event_t, 1> self
) {
  int err;

  err = udx_interface_event_stop(&self->handle);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline void
udx_napi_interface_event_close (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_interface_event_t, 1> self
) {
  int err;

  err = udx_interface_event_close(&self->handle);

  if (err < 0) {
    err = js_throw_error(env, uv_err_name(err), uv_strerror(err));
    assert(err == 0);
  }
}

static inline std::vector<js_object_t>
udx_napi_interface_event_get_addrs (
  js_env_t *env,
  js_typedarray_span_of_t<udx_napi_interface_event_t, 1> self
) {
  int err;

  auto event = &self->handle;

  char ip[INET6_ADDRSTRLEN];
  int family = 0;

  std::vector<js_object_t> result;

  for (int i = 0; i < event->addrs_len; i++) {
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

    js_object_t item;
    err = js_create_object(env, item);
    assert(err == 0);

    err = js_set_property(env, item, "name", addr.name);
    assert(err == 0);

    err = js_set_property(env, item, "host", ip);
    assert(err == 0);

    err = js_set_property(env, item, "family", family);
    assert(err == 0);

    err = js_set_property(env, item, "internal", bool(addr.is_internal));
    assert(err == 0);

    result.push_back(item);
  }

  return result;
}

js_value_t *
udx_native_exports (js_env_t *env, js_value_t *exports) {
  int err;

  // uint32
#define V(name, value) \
  err = js_set_property(env, exports, name, uint32_t(value)); \
  assert(err == 0);

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
  V("sizeof_udx_napi_lookup_t", sizeof(udx_napi_lookup_t));
  V("sizeof_udx_napi_interface_event_t", sizeof(udx_napi_interface_event_t));
  V("sizeof_udx_socket_send_t", sizeof(udx_socket_send_t));
  V("sizeof_udx_stream_send_t", sizeof(udx_stream_send_t));
#undef V

  // functions
#define V(name, fn) \
  err = js_set_property<fn>(env, exports, name); \
  assert(err == 0);

  V("udx_napi_init", udx_napi_init);
  V("udx_napi_socket_init", udx_napi_socket_init);
  V("udx_napi_socket_bind", udx_napi_socket_bind);
  V("udx_napi_socket_set_ttl", udx_napi_socket_set_ttl);
  V("udx_napi_socket_get_recv_buffer_size", udx_napi_socket_get_recv_buffer_size);
  V("udx_napi_socket_set_recv_buffer_size", udx_napi_socket_set_recv_buffer_size);
  V("udx_napi_socket_get_send_buffer_size", udx_napi_socket_get_send_buffer_size);
  V("udx_napi_socket_set_send_buffer_size", udx_napi_socket_set_send_buffer_size);
  V("udx_napi_socket_set_membership", udx_napi_socket_set_membership);
  V("udx_napi_socket_send_ttl", udx_napi_socket_send_ttl);
  V("udx_napi_socket_close", udx_napi_socket_close);
  V("udx_napi_stream_init", udx_napi_stream_init);
  V("udx_napi_stream_set_seq", udx_napi_stream_set_seq);
  V("udx_napi_stream_set_ack", udx_napi_stream_set_ack);
  V("udx_napi_stream_set_mode", udx_napi_stream_set_mode);
  V("udx_napi_stream_connect", udx_napi_stream_connect);
  V("udx_napi_stream_change_remote", udx_napi_stream_change_remote);
  V("udx_napi_stream_relay_to", udx_napi_stream_relay_to);
  V("udx_napi_stream_send", udx_napi_stream_send);
  V("udx_napi_stream_recv_start", udx_napi_stream_recv_start);
  V("udx_napi_stream_write", udx_napi_stream_write);
  V("udx_napi_stream_writev", udx_napi_stream_writev);
  V("udx_napi_stream_write_sizeof", udx_napi_stream_write_sizeof);
  V("udx_napi_stream_write_end", udx_napi_stream_write_end);
  V("udx_napi_stream_destroy", udx_napi_stream_destroy);
  V("udx_napi_lookup", udx_napi_lookup);
  V("udx_napi_interface_event_init", udx_napi_interface_event_init);
  V("udx_napi_interface_event_start", udx_napi_interface_event_start);
  V("udx_napi_interface_event_stop", udx_napi_interface_event_stop);
  V("udx_napi_interface_event_close", udx_napi_interface_event_close);
  V("udx_napi_interface_event_get_addrs", udx_napi_interface_event_get_addrs);
#undef V

  return exports;
}

BARE_MODULE(udx_native, udx_native_exports)
