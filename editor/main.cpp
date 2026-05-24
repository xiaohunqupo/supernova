#include "Backend.h"
#include "cli/ExportCommand.h"

#include <cstring>

using namespace doriax;

int main(int argc, char* argv[]) {
    // Subcommand dispatch: `doriax-editor export ...` routes to the CLI
    // exporter without ever creating a window or starting the GUI loop.
    if (argc >= 2 && std::strcmp(argv[1], "export") == 0) {
        return editor::runExportCommand(argc - 1, argv + 1, argv[0]);
    }

    return editor::Backend::init(argc, argv);
}
