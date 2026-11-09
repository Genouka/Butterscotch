#include "log.h"

#include <stdarg.h>

#ifdef BSDBG
#include "bsdbg_hooks.h"

/* Forward every log line to the ButterscotchDbg session (no-op otherwise).
 * va_copy keeps the caller's va_list usable for platformLog. */
static void bsdbgLogHook(int level, const char* fmt, va_list va) {
    char buffer[4096];
    va_list copy;

    va_copy(copy, va);
    vsnprintf(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);
    buffer[sizeof(buffer) - 1] = '\0';
    BsdbgHook_Log(level, buffer);
}
#define BSDBG_LOG_HOOK(level, fmt, va) bsdbgLogHook((level), (fmt), (va))
#else
#define BSDBG_LOG_HOOK(level, fmt, va) ((void) 0)
#endif

// In the platform main.c
void platformLog(const logType type, const char *format, va_list va);

// Example impl:
// void platformLog(const logType type, const char *format, va_list va) {
//     FILE *out = stderr;
//     switch (type) {
//         case LOG_TYPE_NORMAL:
//             out = stdout;
//             fputs(ANSI_COLOUR_CODE_RESET, out);
//             break;
//         case LOG_TYPE_WARNING:
//             fputs(ANSI_COLOUR_CODE_BOLD_YELLOW"Warning: ", out);
//             break;
//         case LOG_TYPE_ERROR:
//             fputs(ANSI_COLOUR_CODE_BOLD_RED"Error: ", out);
//             break;
// 		case LOG_TYPE_DEBUG:
//             fputs(ANSI_COLOUR_CODE_BOLD_PURPLE"Debug: ", out);
//             break;
//     }
//     vfprintf(out, format, va);
// 	   fputs(ANSI_COLOUR_CODE_RESET, out);
// }

void logInfo(const char* fmt, ...) {
	va_list va;

	va_start(va, fmt);
	BSDBG_LOG_HOOK(1, fmt, va);
	platformLog(LOG_TYPE_NORMAL, fmt, va);
	va_end(va);
}

void vLogInfo(const char* fmt, va_list va) {
	BSDBG_LOG_HOOK(1, fmt, va);
	platformLog(LOG_TYPE_NORMAL, fmt, va);
}

void logWarn(const char* fmt, ...) {
	va_list va;

	va_start(va, fmt);
	BSDBG_LOG_HOOK(2, fmt, va);
	platformLog(LOG_TYPE_WARNING, fmt, va);
	va_end(va);
}

void vLogWarn(const char* fmt, va_list va) {
	BSDBG_LOG_HOOK(2, fmt, va);
	platformLog(LOG_TYPE_WARNING, fmt, va);
}

void logError(const char* fmt, ...) {
	va_list va;

	va_start(va, fmt);
	BSDBG_LOG_HOOK(3, fmt, va);
	platformLog(LOG_TYPE_ERROR, fmt, va);
	va_end(va);
}

void vLogError(const char* fmt, va_list va) {
	BSDBG_LOG_HOOK(3, fmt, va);
	platformLog(LOG_TYPE_ERROR, fmt, va);
}

void logDebug(const char* fmt, ...) {
	va_list va;

	va_start(va, fmt);
	BSDBG_LOG_HOOK(0, fmt, va);
	platformLog(LOG_TYPE_DEBUG, fmt, va);
	va_end(va);
}

void vLogDebug(const char* fmt, va_list va) {
	BSDBG_LOG_HOOK(0, fmt, va);
	platformLog(LOG_TYPE_DEBUG, fmt, va);
}
