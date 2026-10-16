static __thread int value = 73;
int gate_value(void) { return value; }
void gate_set(int v) { value=v; }
