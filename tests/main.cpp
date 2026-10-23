#include "check.h"

int main(int argc, char** argv) { return check::run_all(argc > 1 ? argv[1] : nullptr); }
