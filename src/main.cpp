#include "ui_shell.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
#ifdef __SWITCH__
    socketInitializeDefault();
    nifmInitialize(NifmServiceType_User);
    romfsInit();
    plInitialize(PlServiceType_User);
#endif

    okplayer::SwitchPlayerShell app;
    if (app.init()) {
        app.runMainLoop();
    }
    app.shutdown();

#ifdef __SWITCH__
    plExit();
    romfsExit();
    nifmExit();
    socketExit();
#endif
    return 0;
}
