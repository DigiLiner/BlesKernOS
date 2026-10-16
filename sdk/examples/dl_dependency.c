static int dependency_state;

static void __attribute__((constructor)) blesk_dependency_init(void) {
    dependency_state=41;
}

int blesk_dependency_value(void) { return dependency_state; }
