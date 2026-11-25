/* Roblox's sound on the PS5.
 *
 * FMOD on Android plays through the app's Java class org/fmod/AudioDevice:
 *   init(int channels, int sampleRate, int blockFrames, int blocks) -> boolean
 *       (an AudioTrack: MUSIC stream, 16-bit PCM, streaming mode, playing)
 *   write(byte[] pcm, int length)   - blocking, like AudioTrack.write
 *   close()
 * The fake JNI hands those calls here and they go to the PS5's own audio
 * output (sceAudioOut, 48 kHz, 256-frame grains; sceAudioOutOutput blocks
 * until the previous grain is playing, which paces FMOD's mixer like an
 * AudioTrack does). Other rates are resampled linearly; more than two
 * channels are folded to stereo. ROBLOX_NO_AUDIO=1 keeps FMOD silent. */
#ifndef ROBLOX_AUDIO_H
#define ROBLOX_AUDIO_H

#include <stdint.h>
#include <string.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutClose(int handle);
int sceAudioOutSetVolume(int handle, int flag, int32_t *vol);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);

#define RA_GRAIN 256

static int ra_port = -1;
static int ra_channels = 2;
static int ra_rate = 48000;
static int16_t ra_grain[RA_GRAIN * 2];
static int ra_fill;
static double ra_pos;          /* resampler position in source frames */
static int16_t ra_last[2];     /* last source frame, for interpolation */
static unsigned long long ra_frames_out;

static void
ra_push(int16_t l, int16_t r)
{
   ra_grain[ra_fill * 2] = l;
   ra_grain[ra_fill * 2 + 1] = r;
   if (++ra_fill == RA_GRAIN) {
      sceAudioOutOutput(ra_port, ra_grain);
      ra_fill = 0;
      ra_frames_out += RA_GRAIN;
   }
}

static void
ra_frame(const int16_t *s, int16_t out[2])
{
   if (ra_channels == 1) {
      out[0] = out[1] = s[0];
   } else if (ra_channels == 2) {
      out[0] = s[0], out[1] = s[1];
   } else {
      /* 5.1 / 7.1 (FL FR C LFE ...): fronts plus half the centre and the rest */
      int l = s[0], r = s[1];
      if (ra_channels > 2)
         l += s[2] / 2, r += s[2] / 2;
      for (int c = 4; c < ra_channels; c++) {
         if (c & 1)
            r += s[c] / 2;
         else
            l += s[c] / 2;
      }
      out[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
      out[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
   }
}

static void
ra_write(const uint8_t *pcm, int bytes)
{
   if (ra_port < 0 || !pcm || bytes <= 0)
      return;
   const int frames = bytes / (2 * ra_channels);
   const int16_t *s = (const int16_t *)pcm;
   if (ra_rate == 48000) {
      for (int i = 0; i < frames; i++) {
         int16_t f[2];
         ra_frame(s + i * ra_channels, f);
         ra_push(f[0], f[1]);
      }
      return;
   }
   const double step = (double)ra_rate / 48000.0;
   for (int i = 0; i < frames; i++) {
      int16_t cur[2];
      ra_frame(s + i * ra_channels, cur);
      /* emit every output frame whose position falls in (i-1, i] */
      while (ra_pos <= 1.0) {
         const double t = ra_pos;
         ra_push((int16_t)(ra_last[0] + (cur[0] - ra_last[0]) * t), (int16_t)(ra_last[1] + (cur[1] - ra_last[1]) * t));
         ra_pos += step;
      }
      ra_pos -= 1.0;
      ra_last[0] = cur[0], ra_last[1] = cur[1];
   }
}

/* give the port back when the title ends (it was never closed; after many
 * restarts the system refused new ports) */
static void
ra_close_port(void)
{
   if (ra_port >= 0) {
      sceAudioOutClose(ra_port);
      ra_port = -1;
   }
}

static int
ra_java_hook(const char *key, const char *sig, const uint64_t *args, uint64_t *result)
{
   (void)sig;
   if (strncmp(key, "org/fmod/", 9) != 0)
      return 0;
   if (strcmp(key, "org/fmod/AudioDevice.init") == 0) {
      const int ch = (int)(int32_t)args[0], rate = (int)(int32_t)args[1];
      *result = 0;
      if (getenv("ROBLOX_NO_AUDIO")) {
         report("roblox: audio: off (ROBLOX_NO_AUDIO)");
         return 1;
      }
      if (ch < 1 || ch > 8 || rate < 8000 || rate > 192000) {
         report("roblox: audio: AudioDevice.init(%d ch, %d Hz) refused", ch, rate);
         return 1;
      }
      if (ra_port < 0) {
         static int inited;
         if (!inited) {
            const int r = sceAudioOutInit();
            inited = 1;
            if (r < 0 && (uint32_t)r != 0x8026000Fu) /* already initialised */
               report("roblox: audio: sceAudioOutInit -> 0x%08x", (unsigned)r);
         }
         /* the system user first (it worked for months), then each signed-in
          * user: one session had the system user refused (0x8026000f) for
          * the whole run */
         int users[5] = {0xFF, -1, -1, -1, -1};
         sceUserServiceGetLoginUserIdList(users + 1);
         for (int u = 0; u < 5 && ra_port < 0; u++) {
            if (users[u] == -1)
               continue;
            for (int attempt = 0; attempt < 3 && ra_port < 0; attempt++) {
               ra_port = sceAudioOutOpen(users[u], 0 /* main */, 0, RA_GRAIN, 48000, 1 /* S16 stereo */);
               if (ra_port < 0) {
                  report("roblox: audio: sceAudioOutOpen(user 0x%x) -> 0x%08x", (unsigned)users[u], (unsigned)ra_port);
                  usleep(100 * 1000);
               }
            }
         }
         if (ra_port < 0) {
            report("roblox: audio: no PS5 audio port; no sound");
            ra_port = -1;
            return 1;
         }
         static int closer;
         if (!closer++)
            atexit(ra_close_port);
         int32_t vol[2] = {32768, 32768};
         sceAudioOutSetVolume(ra_port, 3, vol);
      }
      ra_channels = ch;
      ra_rate = rate;
      ra_fill = 0;
      ra_pos = 0;
      ra_last[0] = ra_last[1] = 0;
      report("roblox: audio: FMOD output %d ch %d Hz (block %d x %d) -> PS5 audio port %d", ch, rate,
             (int)(int32_t)args[2], (int)(int32_t)args[3], ra_port);
      *result = 1;
      return 1;
   }
   if (strcmp(key, "org/fmod/AudioDevice.write") == 0) {
      const struct fj_obj *arr = (const struct fj_obj *)(uintptr_t)args[0];
      int len = (int)(int32_t)args[1];
      if (arr && arr->magic == FJ_MAGIC && arr->kind == FJ_ARRAY) {
         if (len > arr->length)
            len = (int)arr->length;
         ra_write((const uint8_t *)arr->data, len);
         static int said;
         if (!said++)
            report("roblox: audio: first %d bytes of sound played", len);
      }
      *result = 0;
      return 1;
   }
   if (strcmp(key, "org/fmod/AudioDevice.close") == 0) {
      report("roblox: audio: FMOD closed its output (%llu frames played)", ra_frames_out);
      *result = 0;
      return 1;
   }
   if (strcmp(key, "org/fmod/FMOD.supportsAAudio") == 0 || strcmp(key, "org/fmod/FMOD.supportsLowLatency") == 0) {
      *result = 0;
      return 1;
   }
   if (strcmp(key, "org/fmod/FMOD.checkInit") == 0) {
      *result = 1;
      return 1;
   }
   return 0;
}

#endif
