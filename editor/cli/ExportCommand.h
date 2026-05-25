#pragma once

namespace doriax::editor {

    // Runs the project export subcommand.
    //
    // `argc` and `argv` should already be shifted past the `export`
    // subcommand token (i.e. argv[0] is the subcommand itself).
    // `executableName` is used only for help and error output.
    int runExportCommand(int argc, char** argv, const char* executableName);

    // Runs the standalone shader generation subcommand.
    int runShadersCommand(int argc, char** argv, const char* executableName);

}
