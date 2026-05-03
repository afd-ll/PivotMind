#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
    #include <windows.h>
    #define OS_WINDOWS 1
    #define OS_UNIX 0
#else
    #include <dlfcn.h>
    #define OS_WINDOWS 0
    #define OS_UNIX 1
#endif

// Platform-specific library loading
#if OS_WINDOWS
    #define LOAD_LIBRARY(path) LoadLibraryA(path)
    #define GET_PROC_ADDRESS(handle, name) GetProcAddress((HMODULE)handle, name)
    #define FREE_LIBRARY(handle) FreeLibrary((HMODULE)handle)
    typedef HMODULE LibraryHandle;
#else
    #define LOAD_LIBRARY(path) dlopen(path, RTLD_LAZY)
    #define GET_PROC_ADDRESS(handle, name) dlsym(handle, name)
    #define FREE_LIBRARY(handle) dlclose(handle)
    typedef void* LibraryHandle;
#endif

#endif // PLATFORM_H
