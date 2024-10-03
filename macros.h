#ifndef NAPI_MACROS
#define NAPI_MACROS

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

#endif
