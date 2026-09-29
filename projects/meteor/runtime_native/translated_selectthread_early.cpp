// Early boot still uses RDSPAF's translated SelectThread until OSCreateThread
// has created the first host fiber. The optimized shard graph excludes the
// title-native address, so compile the translator-produced body once as an
// auxiliary fallback symbol rather than duplicating or editing generated code.
#include "../../../build/meteor/generated/functions/func_80210990.cpp"
