#ifndef NAPI_MACROS
#define NAPI_MACROS

#define NAPI_INIT() \
  static void napi_macros_init(napi_env env, napi_value exports); \
  static napi_value napi_macros_init_wrap(napi_env env, napi_value exports) { \
    napi_macros_init(env, exports); \
    return exports; \
  } \
  NAPI_MODULE(NODE_GYP_MODULE_NAME, napi_macros_init_wrap) \
  static void napi_macros_init(napi_env env, napi_value exports)

#define NAPI_TEST_GC(env) \
  { \
    napi_handle_scope scope; \
    napi_open_handle_scope(env, &scope); \
    napi_value s; \
    napi_value r; \
    napi_create_string_utf8(env, "try { global.gc() } catch {}", NAPI_AUTO_LENGTH, &s); \
    napi_run_script(env, s, &r); \
    napi_close_handle_scope(env, scope); \
  }

#define NAPI_STATUS_THROWS_VOID(call) \
  if ((call) != napi_ok) { \
    napi_throw_error(env, NULL, #call " failed!"); \
    return; \
  }

#define NAPI_STATUS_THROWS(call) \
  if ((call) != napi_ok) { \
    napi_throw_error(env, NULL, #call " failed!"); \
    return NULL; \
  }

#define NAPI_UV_THROWS(err, fn) \
  err = fn; \
  if (err < 0) { \
    napi_throw_error(env, uv_err_name(err), uv_strerror(err)); \
    return NULL; \
  }

#define NAPI_UTF8(name, size, val) \
  char name[size]; \
  size_t name##_len; \
  if (napi_get_value_string_utf8(env, val, (char *) &name, size, &name##_len) != napi_ok) { \
    napi_throw_error(env, "EINVAL", "Expected string"); \
    return NULL; \
  }

#define NAPI_UTF8_MALLOC(name, val) \
  size_t name##_size = 0; \
  NAPI_STATUS_THROWS(napi_get_value_string_utf8(env, val, NULL, 0, &name##_size)) \
  char *name = (char *) malloc((name##_size + 1) * sizeof(char)); \
  size_t name##_len; \
  NAPI_STATUS_THROWS(napi_get_value_string_utf8(env, val, name, name##_size + 1, &name##_len)) \
  name[name##_size] = '\0';

#define NAPI_UINT32(name, val) \
  uint32_t name; \
  if (napi_get_value_uint32(env, val, &name) != napi_ok) { \
    napi_throw_error(env, "EINVAL", "Expected unsigned number"); \
    return NULL; \
  }

#define NAPI_INT32(name, val) \
  int32_t name; \
  if (napi_get_value_int32(env, val, &name) != napi_ok) { \
    napi_throw_error(env, "EINVAL", "Expected number"); \
    return NULL; \
  }

#define NAPI_BUFFER_CAST(type, name, val) \
  type name; \
  size_t name##_len; \
  NAPI_STATUS_THROWS(napi_get_buffer_info(env, val, (void **) &name, &name##_len))

#define NAPI_BUFFER(name, val) \
  NAPI_BUFFER_CAST(char *, name, val)

#define NAPI_FOR_EACH(arr, element) \
  uint32_t arr##_len; \
  napi_get_array_length(env, arr, &arr##_len); \
  napi_value element; \
  for (uint32_t i = 0; i < arr##_len && napi_get_element(env, arr, i, &element) == napi_ok; i++)

#define NAPI_ARGV(n) \
  napi_value argv[n]; \
  size_t argc = n; \
  NAPI_STATUS_THROWS(napi_get_cb_info(env, info, &argc, argv, NULL, NULL))

#define NAPI_ARGV_UTF8(name, size, i) \
  NAPI_UTF8(name, size, argv[i])

#define NAPI_ARGV_UTF8_MALLOC(name, i) \
  NAPI_UTF8_MALLOC(name, argv[i])

#define NAPI_ARGV_UINT32(name, i) \
  NAPI_UINT32(name, argv[i])

#define NAPI_ARGV_INT32(name, i) \
  NAPI_INT32(name, argv[i])

#define NAPI_ARGV_BUFFER_CAST(type, name, i) \
  NAPI_BUFFER_CAST(type, name, argv[i])

#define NAPI_ARGV_BUFFER(name, i) \
  NAPI_ARGV_BUFFER_CAST(char *, name, i)

#endif
