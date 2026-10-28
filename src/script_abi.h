#pragma once
// The game's script-native call ABI (C, notes/NATIVE-BRIDGE.md §2). A native handler is
// cdecl(vm, descriptor); its EAX is ignored. The argument arrays point into the live VM stacks:
// read them, and rewrite a value (keeping a type the VM can release) only before calling on.
#include <stdint.h>

enum ScriptType : uint8_t {
    kTypeNone = 0, kTypeFloat = 1, kTypeInt = 2, kTypeHash = 3, kTypeHeapString = 5, kTypeLiteralString = 6, kTypeObject = 7,
};

struct NativeCall {
    uint32_t id;
    uint8_t normalCall;
    uint8_t pad[3];
    uint8_t* types;
    uint32_t* values;
    uint32_t argc;
    uint32_t retValue;
    uint32_t retType;
};
static_assert(sizeof(void*) == 4, "x86 only");

typedef void(__cdecl* NativeFn)(void* vm, NativeCall* d);
