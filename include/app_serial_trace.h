#pragma once
// Diagnostika na USB Serial. Default OFF — zapni: -DAPP_SERIAL_TRACE=1
#ifndef APP_SERIAL_TRACE
#define APP_SERIAL_TRACE 0
#endif
#if APP_SERIAL_TRACE
#define APP_SLOG(...)                                                          \
  do {                                                                         \
    Serial.printf(__VA_ARGS__);                                                \
  } while (0)
#define APP_SLOG_LN(msg)                                                       \
  do {                                                                         \
    Serial.println(msg);                                                       \
  } while (0)
#else
#define APP_SLOG(...) ((void)0)
#define APP_SLOG_LN(msg) ((void)0)
#endif
