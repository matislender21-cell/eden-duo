// stb_vorbis's implementation in a translation unit of its own (its short internal macro names
// would clash with ours): decodes the game's own OGG sounds on the device.
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
