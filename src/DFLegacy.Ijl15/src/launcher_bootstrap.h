#pragma once

namespace dfl::launcher_bootstrap
{
    // Folds what DFLegacy.Launcher used to do into the client process itself:
    // rewrites the process command line with the entrance arguments the client
    // expects, and starts the emulator when nothing is listening yet.
    //
    // Called from DllMain during DLL_PROCESS_ATTACH, which runs before the
    // client's own entry point, so the patched command line is what the client
    // parses. Every failure is non-fatal: the client still starts and simply
    // shows its normal login screen.
    void Apply() noexcept;
}
