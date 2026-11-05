#pragma once
LONG DetourTransactionBegin();
LONG DetourUpdateThread(void*);
LONG DetourAttach(void**, void*);
LONG DetourTransactionCommit();
LONG DetourTransactionAbort();
