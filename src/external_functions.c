#include "external_functions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runner.h"
#include "utils.h"
#include "stb_ds.h"

// Runtime dynamic-library loading for external_define()/external_call().
// Consoles and web builds do not expose a usable dlopen-equivalent, so they are
// reported as unsupported and the builtins log a TODO instead.

#if !defined(_WIN32) && (defined(PLATFORM_PS2) || defined(PLATFORM_PS3) || defined(PLATFORM_VITA) || \
                         defined(__vita__) || defined(__SWITCH__) || defined(__EMSCRIPTEN__) || \
                         defined(PLATFORM_ANDROID) || defined(PLATFORM_WEB))
#define BS_EXTERNAL_UNSUPPORTED 1
#else
#define BS_EXTERNAL_UNSUPPORTED 0
#endif

#if defined(_WIN32)
#include <windows.h>
#ifndef IMAGE_FILE_MACHINE_ARM64
#define IMAGE_FILE_MACHINE_ARM64 0xAA64
#endif
#ifndef ERROR_EXE_MACHINE_TYPE_MISMATCH
#define ERROR_EXE_MACHINE_TYPE_MISMATCH 216
#endif
#elif !BS_EXTERNAL_UNSUPPORTED && (defined(__unix__) || defined(__APPLE__))
#include <dlfcn.h>
#elif !BS_EXTERNAL_UNSUPPORTED
#define BS_EXTERNAL_UNSUPPORTED 1
#endif

// ===[ Architecture Diagnostics ]===

#if defined(_WIN32)
static const char* externalHostArchName(void) {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64 (64-bit)";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86 (32-bit)";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "ARM64 (64-bit)";
#elif defined(_M_ARM) || defined(__arm__)
    return "ARM (32-bit)";
#else
    return "unknown-architecture";
#endif
}

static const char* externalPeMachineName(uint16_t machine) {
    switch (machine) {
        case IMAGE_FILE_MACHINE_I386: return "x86 (32-bit)";
        case IMAGE_FILE_MACHINE_AMD64: return "x86_64 (64-bit)";
        case IMAGE_FILE_MACHINE_ARM: return "ARM (32-bit)";
        case IMAGE_FILE_MACHINE_ARMNT: return "ARM Thumb-2 (32-bit)";
        case IMAGE_FILE_MACHINE_ARM64: return "ARM64 (64-bit)";
        default: return "unsupported-architecture";
    }
}

// Reads the PE COFF machine type without loading the image. Returns false for
// non-PE files (including 16-bit DLLs) or unreadable paths.
static bool externalReadPeMachine(const char* path, uint16_t* outMachine) {
    unsigned char dosHeader[64];
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    bool ok = false;
    if (fread(dosHeader, 1, sizeof(dosHeader), file) == sizeof(dosHeader) &&
        dosHeader[0] == 'M' && dosHeader[1] == 'Z') {
        int32_t peOffset = (int32_t) ((uint32_t) dosHeader[0x3C] |
                                      ((uint32_t) dosHeader[0x3D] << 8) |
                                      ((uint32_t) dosHeader[0x3E] << 16) |
                                      ((uint32_t) dosHeader[0x3F] << 24));
        unsigned char peHeader[6];
        if (peOffset > 0 && fseek(file, peOffset, SEEK_SET) == 0 &&
            fread(peHeader, 1, sizeof(peHeader), file) == sizeof(peHeader) &&
            peHeader[0] == 'P' && peHeader[1] == 'E' && peHeader[2] == 0 && peHeader[3] == 0) {
            *outMachine = (uint16_t) (peHeader[4] | (peHeader[5] << 8));
            ok = true;
        }
    }
    fclose(file);
    return ok;
}
#endif

// ===[ Library Loading ]===

bool External_platformSupported(void) {
    return !BS_EXTERNAL_UNSUPPORTED;
}

void* External_openLibrary(const char* name, char* error, size_t errorSize) {
    if (error != NULL && errorSize > 0) error[0] = '\0';
#if BS_EXTERNAL_UNSUPPORTED
    (void) name;
    if (error != NULL && errorSize > 0) snprintf(error, errorSize, "dynamic library loading is not supported on this platform");
    return NULL;
#elif defined(_WIN32)
    // Game extension DLLs usually sit next to their own dependencies (bass.dll,
    // CRT/runtime DLLs shipped with the game, ...). Plain LoadLibraryA resolves
    // those through the host process' search order - runner executable directory,
    // system directories, current directory, PATH - never through the loaded
    // DLL's directory, so a game started from a different working directory
    // fails with ERROR_MOD_NOT_FOUND (126). For absolute paths, ask the loader
    // to search the DLL's directory first, which is how GameMaker's runner
    // loads extension DLLs. Bare names keep the system search path; the plain
    // call stays as a fallback for paths the altered search rejects.
    //
    // The file system layer joins game paths with '/', but the loader only
    // honours LOAD_WITH_ALTERED_SEARCH_PATH for backslash paths: with '/' it
    // silently falls back to the default search and the dependency lookup
    // fails again. Normalise a copy for the loader only.
    char* normalizedName = NULL;
    if (strchr(name, '/') != NULL) {
        size_t nameLength = strlen(name);
        normalizedName = (char*) safeMalloc(nameLength + 1);
        for (size_t i = 0; i <= nameLength; i++)
            normalizedName[i] = name[i] == '/' ? '\\' : name[i];
    }
    const char* loadName = normalizedName != NULL ? normalizedName : name;

    bool absolutePath = loadName[0] != '\0' &&
        ((((loadName[0] >= 'a' && loadName[0] <= 'z') || (loadName[0] >= 'A' && loadName[0] <= 'Z')) && loadName[1] == ':') ||
         loadName[0] == '\\' || loadName[0] == '/');
    HMODULE library = absolutePath
        ? LoadLibraryExA(loadName, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)
        : LoadLibraryA(loadName);
    if (library == NULL && absolutePath) library = LoadLibraryA(loadName);
    if (library != NULL) {
        free(normalizedName);
        return (void*) library;
    }

    DWORD lastError = GetLastError();
    uint16_t machine = 0;
    bool isArchError = lastError == ERROR_BAD_EXE_FORMAT || lastError == ERROR_EXE_MACHINE_TYPE_MISMATCH;
    if (error != NULL && errorSize > 0) {
        if (isArchError && externalReadPeMachine(loadName, &machine)) {
            snprintf(error, errorSize,
                     "architecture mismatch: the DLL is %s but this Butterscotch build is %s "
                     "(Windows error %lu); run the matching-architecture build",
                     externalPeMachineName(machine), externalHostArchName(), (unsigned long) lastError);
        } else if (isArchError) {
            snprintf(error, errorSize,
                     "not a valid DLL for this %s build (Windows error %lu)",
                     externalHostArchName(), (unsigned long) lastError);
        } else if (lastError == ERROR_MOD_NOT_FOUND) {
            snprintf(error, errorSize,
                     "Windows error %lu (a DLL this module depends on was not found next to it or on the search path)",
                     (unsigned long) lastError);
        } else {
            snprintf(error, errorSize, "Windows error %lu", (unsigned long) lastError);
        }
    }
    free(normalizedName);
    return NULL;
#else
    void* library = dlopen(name, RTLD_NOW);
    if (library == NULL && error != NULL && errorSize > 0) {
        const char* detail = dlerror();
        snprintf(error, errorSize, "%s", detail != NULL ? detail : "dlopen failed");
    }
    return library;
#endif
}

void* External_getSymbol(void* library, const char* name) {
#if BS_EXTERNAL_UNSUPPORTED
    (void) library;
    (void) name;
    return NULL;
#elif defined(_WIN32)
    return (void*) (uintptr_t) GetProcAddress((HMODULE) library, name);
#else
    return dlsym(library, name);
#endif
}

// x86 Windows DLLs decorate exported symbols:
//   __cdecl   -> _name
//   __stdcall -> _name@N (MSVC) or name@N (MinGW), where N is the stack byte count
// 64-bit Windows has a single calling convention and no decoration.
void* External_getSymbolEx(void* library, const char* name, int32_t callType, int32_t argCount, const int32_t* argTypes) {
    void* symbol = External_getSymbol(library, name);
    if (symbol != NULL) return symbol;

#if defined(_WIN32) && !defined(_WIN64)
    size_t nameLength = strlen(name);
    char* decorated = (char*) safeMalloc(nameLength + 32);
    snprintf(decorated, nameLength + 32, "_%s", name);
    symbol = External_getSymbol(library, decorated);
    if (symbol == NULL && callType == 1) {
        // A double occupies 8 stack bytes on x86; a char* occupies 4.
        int32_t stackBytes = 0;
        for (int32_t i = 0; i < argCount; i++) {
            stackBytes += argTypes[i] == 1 ? 4 : 8;
        }
        snprintf(decorated, nameLength + 32, "_%s@%d", name, stackBytes);
        symbol = External_getSymbol(library, decorated);
        if (symbol == NULL) {
            snprintf(decorated, nameLength + 32, "%s@%d", name, stackBytes);
            symbol = External_getSymbol(library, decorated);
        }
    }
    free(decorated);
#else
    (void) callType;
    (void) argCount;
    (void) argTypes;
#endif
    return symbol;
}

void External_closeLibrary(void* library) {
#if BS_EXTERNAL_UNSUPPORTED
    (void) library;
#elif defined(_WIN32)
    if (library != NULL) FreeLibrary((HMODULE) library);
#else
    if (library != NULL) dlclose(library);
#endif
}

// ===[ Invocation ]===

#ifdef _WIN32
#define EXT_CDECL __cdecl
#define EXT_STDCALL __stdcall
#else
#define EXT_CDECL
#define EXT_STDCALL
#endif

// GameMaker documents that external functions with 4+ arguments must use ty_real
// for every argument, so only the 0..3 argument cases need mixed-type dispatch.
// mask bit i is set when argument i is ty_string (matches builtin_external_call).
#if defined(_MSC_VER) && defined(_M_IX86)
/* GameMaker tolerates a declared calling convention that does not match the
 * DLL (Mvz2's MaizeMusic.dll is cdecl but declares dll_stdcall), and a plain
 * __stdcall call through such a function leaves the arguments on the stack,
 * which corrupts the caller. Remember ESP around the call and restore it: for
 * a genuine __stdcall callee the restore is a no-op, for a __cdecl callee it
 * undoes the leftover arguments. */
#define EXT_STACK_SAVE(var) do { __asm { mov var, esp }; } while (0)
#define EXT_STACK_RESTORE(var) do { __asm { mov esp, var }; } while (0)
#else
#define EXT_STACK_SAVE(var) ((void) 0)
#define EXT_STACK_RESTORE(var) ((void) 0)
#endif

#define EXT_CALL_CASE(CONV, n, mask, TYPES, ARGS) \
    case (((n) << 8) | (mask)): { \
        void* extStackBase = NULL; \
        EXT_STACK_SAVE(extStackBase); \
        if (def->returnType == 1) { \
            char* result = ((char* (CONV *) TYPES) def->symbol) ARGS; \
            EXT_STACK_RESTORE(extStackBase); \
            *outString = safeStrdup(result != NULL ? result : ""); \
            *outReal = 0.0; \
        } else { \
            *outReal = (double) ((double (CONV *) TYPES) def->symbol) ARGS; \
            EXT_STACK_RESTORE(extStackBase); \
            *outString = NULL; \
        } \
        return true; \
    }

#define EXT_CALL_CASES(CONV) \
    EXT_CALL_CASE(CONV, 0, 0, (void), ()) \
    EXT_CALL_CASE(CONV, 1, 0, (double), (d[0])) \
    EXT_CALL_CASE(CONV, 1, 1, (char*), (s[0])) \
    EXT_CALL_CASE(CONV, 2, 0, (double, double), (d[0], d[1])) \
    EXT_CALL_CASE(CONV, 2, 1, (char*, double), (s[0], d[1])) \
    EXT_CALL_CASE(CONV, 2, 2, (double, char*), (d[0], s[1])) \
    EXT_CALL_CASE(CONV, 2, 3, (char*, char*), (s[0], s[1])) \
    EXT_CALL_CASE(CONV, 3, 0, (double, double, double), (d[0], d[1], d[2])) \
    EXT_CALL_CASE(CONV, 3, 1, (char*, double, double), (s[0], d[1], d[2])) \
    EXT_CALL_CASE(CONV, 3, 2, (double, char*, double), (d[0], s[1], d[2])) \
    EXT_CALL_CASE(CONV, 3, 3, (char*, char*, double), (s[0], s[1], d[2])) \
    EXT_CALL_CASE(CONV, 3, 4, (double, double, char*), (d[0], d[1], s[2])) \
    EXT_CALL_CASE(CONV, 3, 5, (char*, double, char*), (s[0], d[1], s[2])) \
    EXT_CALL_CASE(CONV, 3, 6, (double, char*, char*), (d[0], s[1], s[2])) \
    EXT_CALL_CASE(CONV, 3, 7, (char*, char*, char*), (s[0], s[1], s[2])) \
    EXT_CALL_CASE(CONV, 4, 0, (double, double, double, double), (d[0], d[1], d[2], d[3])) \
    EXT_CALL_CASE(CONV, 5, 0, (double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4])) \
    EXT_CALL_CASE(CONV, 6, 0, (double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5])) \
    EXT_CALL_CASE(CONV, 7, 0, (double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6])) \
    EXT_CALL_CASE(CONV, 8, 0, (double, double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7])) \
    EXT_CALL_CASE(CONV, 9, 0, (double, double, double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8])) \
    EXT_CALL_CASE(CONV, 10, 0, (double, double, double, double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9])) \
    EXT_CALL_CASE(CONV, 11, 0, (double, double, double, double, double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10])) \
    EXT_CALL_CASE(CONV, 12, 0, (double, double, double, double, double, double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11])) \
    EXT_CALL_CASE(CONV, 13, 0, (double, double, double, double, double, double, double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12])) \
    EXT_CALL_CASE(CONV, 14, 0, (double, double, double, double, double, double, double, double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13])) \
    EXT_CALL_CASE(CONV, 15, 0, (double, double, double, double, double, double, double, double, double, double, double, double, double, double, double), (d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14]))


static bool extDispatchCdecl(const ExternalFunction* def, const double* d, char* const* s, int32_t mask,
                              double* outReal, char** outString) {
    switch (((def->argCount) << 8) | (mask)) {
        EXT_CALL_CASES(EXT_CDECL)
        default:
            return false;
    }
}

#ifdef _WIN32
static bool extDispatchStdcall(const ExternalFunction* def, const double* d, char* const* s, int32_t mask,
                               double* outReal, char** outString) {
    switch (((def->argCount) << 8) | (mask)) {
        EXT_CALL_CASES(EXT_STDCALL)
        default:
            return false;
    }
}
#else
#define extDispatchStdcall extDispatchCdecl
#endif

bool External_invokeRaw(const ExternalFunction* def, const double* realArgs, char* const* stringArgs,
                        int32_t stringMask, double* outReal, char** outString) {
    if (def == NULL || def->symbol == NULL) return false;
    return def->callType == 1
        ? extDispatchStdcall(def, realArgs, stringArgs, stringMask, outReal, outString)
        : extDispatchCdecl(def, realArgs, stringArgs, stringMask, outReal, outString);
}

// ===[ Definition Lifetime ]===

char* External_resolveLibraryPath(Runner* runner, const char* dllName) {
    if (runner == NULL || dllName == NULL) return NULL;
    FileSystem* fs = runner->fileSystem;
    if (fs == NULL || fs->vtable == NULL || fs->vtable->fileExists == NULL) return NULL;
    if (!fs->vtable->fileExists(fs, dllName)) return NULL;
    if (fs->vtable->resolveReadPath != NULL) return fs->vtable->resolveReadPath(fs, dllName);
    if (fs->vtable->resolvePath != NULL) return fs->vtable->resolvePath(fs, dllName);
    return NULL;
}

int32_t External_define(Runner* runner, const char* dllName, const char* functionName,
                        int32_t callType, int32_t returnType, int32_t argCount, const int32_t* argTypes) {
    if (runner == NULL || dllName == NULL || functionName == NULL) return -1;
    if (argCount < 0 || argCount > EXTERNAL_MAX_ARGS) {
        logWarn("[external_define] Invalid argument count %d for '%s'\n", argCount, functionName);
        return -1;
    }
    if (!External_platformSupported()) {
        logWarn("[external_define] TODO: runtime dynamic library loading is not implemented on this platform\n");
        return -1;
    }

    ExternalFunction* definition = (ExternalFunction*) safeCalloc(1, sizeof(ExternalFunction));
    definition->callType = callType == 1 ? 1 : 0;
    definition->returnType = returnType == 1 ? 1 : 0;
    definition->argCount = argCount;
    definition->dllName = safeStrdup(dllName);
    definition->functionName = safeStrdup(functionName);
    for (int32_t i = 0; i < argCount; i++) {
        definition->argTypes[i] = argTypes[i] == 1 ? 1 : 0;
        if (argCount >= 4 && definition->argTypes[i] != 0) {
            logWarn("[external_define] '%s' has 4+ arguments but argument %d is not ty_real\n", functionName, i);
        }
    }

    // Prefer the save area (the writable, mod-friendly location) over the bundle, matching
    // how GML file reads resolve. A NULL result means the name is in neither area, so the raw
    // name reaches the OS loader and system DLLs still resolve through the normal search path.
    char* resolvedDllName = External_resolveLibraryPath(runner, dllName);
    const char* libraryPath = resolvedDllName != NULL ? resolvedDllName : dllName;

    char loadError[512];
    definition->library = External_openLibrary(libraryPath, loadError, sizeof(loadError));
    if (definition->library == NULL) {
        logWarn("[external_define] Failed to load library '%s': %s\n", libraryPath, loadError);
        free(resolvedDllName);
        free(definition->dllName);
        free(definition->functionName);
        free(definition);
        return -1;
    }
    logInfo("[external_define] Loaded library '%s'\n", libraryPath);
    free(resolvedDllName);

    // 32-bit Windows DLLs export stdcall functions as _name@N / name@N; External_getSymbolEx
    // retries those decorations using the declared argument types.
    definition->symbol = External_getSymbolEx(definition->library, functionName, definition->callType,
                                              argCount, definition->argTypes);
    if (definition->symbol == NULL) {
        logWarn("[external_define] Failed to find symbol '%s' in '%s'%s\n", functionName, dllName,
                definition->callType == 1 ? " (also tried 32-bit stdcall decorations)" : "");
        External_closeLibrary(definition->library);
        free(definition->dllName);
        free(definition->functionName);
        free(definition);
        return -1;
    }

    int32_t id = (int32_t) arrlen(runner->externalFunctions);
    arrput(runner->externalFunctions, definition);
    logInfo("[external_define] '%s' -> id %d\n", functionName, id);
    return id;
}

bool External_free(Runner* runner, int32_t id) {
    if (runner == NULL || id < 0 || id >= (int32_t) arrlen(runner->externalFunctions)) return false;
    ExternalFunction* function = runner->externalFunctions[id];
    if (function == NULL) return false;
    External_closeLibrary(function->library);
    free(function->dllName);
    free(function->functionName);
    free(function);
    runner->externalFunctions[id] = NULL;
    return true;
}

void External_freeAll(Runner* runner) {
    if (runner == NULL || runner->externalFunctions == NULL) return;

    repeat((int32_t) arrlen(runner->externalFunctions), i) {
        ExternalFunction* function = runner->externalFunctions[i];
        if (function == NULL) continue;
        External_closeLibrary(function->library);
        free(function->dllName);
        free(function->functionName);
        free(function);
    }
    arrfree(runner->externalFunctions);
    runner->externalFunctions = NULL;
}
