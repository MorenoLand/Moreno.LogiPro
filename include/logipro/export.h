#pragma once

#if defined(_WIN32)
#if defined(LOGIPRO_BUILD_DLL)
#define LOGIPRO_C_API __declspec(dllexport)
#elif defined(LOGIPRO_USE_DLL)
#define LOGIPRO_C_API __declspec(dllimport)
#else
#define LOGIPRO_C_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define LOGIPRO_C_API __attribute__((visibility("default")))
#else
#define LOGIPRO_C_API
#endif
