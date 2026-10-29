// The end-to-end test's stand-in for ChaoGarage's start: runs the loader's own step that puts staged mod updates in place
// (A:\ChaoGarage\src\host\pending_updates.h, 1.3.0), on a fake game's mods folder, and prints one line per result.
// usage: loader_start_step.exe <game>\mods
#include <stdio.h>
#include "pending_updates.h"

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    for (auto& r : ssasr::apply_pending_updates(ssasr::full(argv[1])))
        wprintf(L"%s %s %s: %s\n", r.applied ? L"APPLIED" : L"NOT-APPLIED", r.folder.c_str(), r.version.c_str(), r.detail.c_str());
    return 0;
}
