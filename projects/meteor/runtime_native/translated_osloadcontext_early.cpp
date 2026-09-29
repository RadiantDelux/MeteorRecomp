// Early boot still uses RDSPAF's translated OSLoadContext until OSCreateThread
// has created the first host fiber. The optimized build-shard graph does not
// export the standalone translated symbol, so compile the translator-produced
// body once as a title-local auxiliary source rather than duplicating or editing
// generated code.
#include "../../../build/meteor/generated/functions/func_80209E24.cpp"
