#pragma once

#if defined(_WIN32)
#if defined(LOGIPRO_BUILD_DLL)
#define LOGIPRO_API __declspec(dllexport)
#elif defined(LOGIPRO_USE_DLL)
#define LOGIPRO_API __declspec(dllimport)
#else
#define LOGIPRO_API
#endif
#else
#define LOGIPRO_API
#endif
