#ifndef _BS_EXTERNAL_FUNCTIONS_H_
#define _BS_EXTERNAL_FUNCTIONS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Runner Runner;

// Runtime definition created by GML's external_define(). The symbol is resolved eagerly;
// external_call() marshals GML values according to argTypes.
// Kept here (rather than runner.h) so the loading/dispatch code can be built on its own.
typedef struct {
    void* library;
    void* symbol;
    int32_t callType;   // 0 = dll_cdecl, 1 = dll_stdcall
    int32_t returnType; // 0 = ty_real, 1 = ty_string
    int32_t argCount;
    int32_t argTypes[16]; // 0 = ty_real, 1 = ty_string
    char* dllName;
    char* functionName;
} ExternalFunction;

// Upper bound accepted by external_define() (GameMaker's documented 0..15 range).
#define EXTERNAL_MAX_ARGS 15

// Returns true when this platform can load shared libraries at runtime.
bool External_platformSupported(void);
// Loads a DLL/dylib. Returns NULL on failure and fills `error` with a human-readable
// reason (including architecture mismatches on Windows).
void* External_openLibrary(const char* name, char* error, size_t errorSize);
// Resolves a symbol inside a library by its exact name. Returns NULL on failure.
void* External_getSymbol(void* library, const char* name);
// Resolves a symbol, retrying the compiler decorations used by 32-bit Windows DLLs
// (`_name` for cdecl, `_name@N` / `name@N` for stdcall, with N = stack bytes).
// `callType` and `argTypes` describe the declared signature so the stdcall suffix can be computed.
void* External_getSymbolEx(void* library, const char* name, int32_t callType, int32_t argCount, const int32_t* argTypes);
// Closes a library previously returned by External_openLibrary().
void External_closeLibrary(void* library);

// Resolves a DLL path against the runner's sandboxed filesystem: the save area is checked
// first, then the game bundle. Returns NULL when the file is in neither area (the caller
// should then pass the original name through so system DLLs still resolve).
char* External_resolveLibraryPath(Runner* runner, const char* dllName);

// Marshals and invokes the external function. On success, exactly one of *outReal /
// *outString is set (outString is heap-allocated). Returns false for an unsupported
// argument signature (the caller should log and return a default value).
bool External_invokeRaw(const ExternalFunction* def, const double* realArgs, char* const* stringArgs,
                        int32_t stringMask, double* outReal, char** outString);

// Releases one external_define() entry. The id stays valid but becomes a NULL slot.
bool External_free(Runner* runner, int32_t id);
// Frees every external_define() entry owned by the runner.
void External_freeAll(Runner* runner);

// Implements external_define(): resolves the library through the sandbox, loads it,
// resolves the symbol (including 32-bit decorations) and stores the definition.
// Returns the new id, or -1 on failure. The name strings are copied.
int32_t External_define(Runner* runner, const char* dllName, const char* functionName,
                        int32_t callType, int32_t returnType, int32_t argCount, const int32_t* argTypes);

#endif /* _BS_EXTERNAL_FUNCTIONS_H_ */
