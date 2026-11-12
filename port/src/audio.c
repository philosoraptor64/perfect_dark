#include <PR/ultratypes.h>
#include <stdio.h>
#include <SDL.h>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "system.h"

static SDL_AudioDeviceID dev;
static const s16 *nextBuf;
static u32 nextSize = 0;

static s32 bufferSize = 512;
static s32 queueLimit = 8192;

/* i64 module: audio goes to the host (n64_host.audio_write, 22020 Hz stereo) */
static void (*s_HostWrite)(const short *stereo, unsigned frames, unsigned rate);
static unsigned (*s_HostBuffered)(unsigned rate);
void audioSetHostOutput(void (*write)(const short *, unsigned, unsigned), unsigned (*buffered)(unsigned))
{
	s_HostWrite = write;
	s_HostBuffered = buffered;
}

void audioShutdown(void)
{
	if (dev) {
		SDL_CloseAudioDevice(dev);
		dev = 0;
	}
}

s32 audioInit(void)
{
	if (s_HostWrite) {
		sysLogPrintf(LOG_NOTE, "audio: output through the host (i64)");
		return 0;
	}
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 22020; // TODO: this might cause trouble for some platforms
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = bufferSize;
	want.callback = NULL;

	nextBuf = NULL;

	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (dev == 0) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudio error: %s", SDL_GetError());
		return -1;
	}

	SDL_PauseAudioDevice(dev, 0);

	return 0;
}

s32 audioGetBytesBuffered(void)
{
	if (s_HostWrite) return s_HostBuffered ? (s32)(s_HostBuffered(22020) * 4) : 0;
	return dev ? SDL_GetQueuedAudioSize(dev) : 0;
}

s32 audioGetSamplesBuffered(void)
{
	return audioGetBytesBuffered() / 4;
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
	nextBuf = buf;
	nextSize = len;
}

void audioEndFrame(void)
{
	if (nextBuf && nextSize) {
		if (audioGetSamplesBuffered() < queueLimit) {
			if (s_HostWrite) s_HostWrite(nextBuf, nextSize / 4, 22020);
			else if (dev) SDL_QueueAudio(dev, nextBuf, nextSize);
		}
		nextBuf = NULL;
		nextSize = 0;
	}
}

PD_CONSTRUCTOR static void audioConfigInit(void)
{
	configRegisterInt("Audio.BufferSize", &bufferSize, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 1 * 1024 * 1024);
}
