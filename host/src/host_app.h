// host_app.h — точки входа двух режимов (диспетчер в entry.cpp)
#pragma once
int run_gui();                       // GUI-режим (main.cpp)
int run_cli(int argc, char** argv);  // консольный режим (cli.cpp)
