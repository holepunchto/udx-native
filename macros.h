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

#define NAPI_FOR_EACH(arr, element) \
  uint32_t arr##_len; \
  napi_get_array_length(env, arr, &arr##_len); \
  napi_value element; \
  for (uint32_t i = 0; i < arr##_len && napi_get_element(env, arr, i, &element) == napi_ok; i++)

#endif
