// entry.cpp — диспетчер двойного режима. Общий для gammapult.exe и gammapult.com.
//   GAMMAPULT_CONSOLE задан → бинарь .com: всегда CLI.
//   иначе (.exe, WINDOWS subsystem): есть подкоманда → CLI (с привязкой к
//   родительской консоли), иначе → GUI.
#include "host_app.h"
#include <cstring>

static bool is_subcommand(const char* a)
{
    static const char* subs[] = { "list", "info", "acquire", "enc", "cmd",
                                  "help", "--help", "-h" };
    for (const char* s : subs)
        if (std::strcmp(a, s) == 0) return true;
    return false;
}

#ifdef GAMMAPULT_CONSOLE

int main(int argc, char** argv) { return run_cli(argc, argv); }

#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc > 1 && is_subcommand(argv[1])) {
        // .exe запущен с подкомандой из консоли — пишем в родительскую консоль
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            freopen_s(&f, "CONIN$",  "r", stdin);
        }
        return run_cli(argc, argv);
    }
    return run_gui();
}

#endif
