# min-alut

From ca. June of 2021. Minimal version of ALUT, with around 10% the code size of [FreeALUT](https://github.com/vancegroup/freealut) and the same functionality (minus the file loading, but I'm sure you can use stdio just fine on your own). I just took FreeALUT and reduced the object bloat, as its basically just two static functions - a basic synthesizer and a decoder of wave forms into raw OpenAL streams. I include [alad](https://github.com/hypatia-of-sva/alad) here, but you can also just include <AL/al.h> and <AL/alc.h>, the code doesn't change after the first line.
