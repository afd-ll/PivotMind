#ifdef _WIN32
#include "../include/network_tool.h"

#pragma comment(lib, "winhttp.lib")

static int g_network_initialized = 0;
