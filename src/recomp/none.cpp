// Used when no recompiled code has been generated yet: everything runs on the interpreter.
#include <vector>

#include "recomp/runtime.h"

void registerRecompiled(std::vector<rc::Entry>&) {}
