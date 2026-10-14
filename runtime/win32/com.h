// Host-implemented COM objects (DirectDraw, DirectSound) for guest code.
//
// An object is 8 bytes of guest memory: a vtable pointer and a reference count. The vtable is an
// array of HLE thunks "Interface!Method" in declaration order; COMMETHOD implements one.
#pragma once

#include <initializer_list>
#include <string>

#include "hle.h"

bool comRegisterInterface(const char* name, std::initializer_list<const char*> methods);
uint32_t comNew(const char* iface); // returns the guest COM pointer

#define COM_INTERFACE(name, ...) [[maybe_unused]] static const bool comreg_##name = comRegisterInterface(#name, { __VA_ARGS__ })
#define COMMETHOD(iface, method, nargs) WIN32API(iface, method, nargs)
