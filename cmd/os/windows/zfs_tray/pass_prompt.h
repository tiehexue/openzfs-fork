#pragma once
#include <windows.h>
#include "rpc_client.h" // zrpc_t

BOOL PromptPassphrase(HWND owner, const wchar_t *datasetW,
    uint8_t **out, uint32_t *outlen);
