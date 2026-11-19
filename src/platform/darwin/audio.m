#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include <sched.h>
#include <stdatomic.h>

#include "voice/audio.h"

constexpr AudioUnitElement SPEAKER = 0;
constexpr AudioUnitElement MICROPHONE = 1;
constexpr UInt32 FRAMES_MAX = 4096;

struct Audio {
  AudioEvents e;
  AudioUnit unit;
  float heard[FRAMES_MAX];
  // audio_stop was called: the callbacks touch nothing of their owner's
  // anymore, and `busy` counts the ones that still do.
  atomic_bool stopped;
  atomic_int busy;
  atomic_bool live;
  char *input, *output;
  bool raw;
};

// the unit takes long to start and to stop, a tenth of a second and more:
// both happen here, in the order they were asked, never on the main thread.
static dispatch_queue_t units(void) {
  static dispatch_queue_t queue;
  static dispatch_once_t once;

  dispatch_once(&once, ^{
    queue = dispatch_queue_create("quarrel.audio", DISPATCH_QUEUE_SERIAL);
  });

  return queue;
}

static bool enter(Audio *a) {
  atomic_fetch_add(&a->busy, 1);

  if (atomic_load(&a->stopped)) {
    atomic_fetch_sub(&a->busy, 1);
    return false;
  }

  return true;
}

static void leave(Audio *a) {
  atomic_fetch_sub(&a->busy, 1);
}

void audio_ask(void (*done)(void *ctx, bool allowed), void *ctx) {
  AVAuthorizationStatus status =
    [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];

  if (status != AVAuthorizationStatusNotDetermined) {
    done(ctx, status == AVAuthorizationStatusAuthorized);
    return;
  }

  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                           completionHandler:^(BOOL granted) {
                             dispatch_async(dispatch_get_main_queue(), ^{
                               done(ctx, granted);
                             });
                           }];
}

static OSStatus on_microphone(void *ctx, AudioUnitRenderActionFlags *flags,
                              const AudioTimeStamp *when, UInt32, UInt32 frames,
                              AudioBufferList *) {
  Audio *a = ctx;

  if (frames > FRAMES_MAX) {
    return noErr;
  }

  AudioBufferList list = {
    .mNumberBuffers = 1,
    .mBuffers[0] = {1, frames * sizeof(float), a->heard},
  };

  if (AudioUnitRender(a->unit, flags, when, MICROPHONE, frames, &list) ==
        noErr &&
      enter(a)) {
    atomic_store_explicit(&a->live, true, memory_order_relaxed);
    a->e.captured(a->e.ctx, a->heard, frames);
    leave(a);
  }

  return noErr;
}

static OSStatus on_speaker(void *ctx, AudioUnitRenderActionFlags *,
                           const AudioTimeStamp *, UInt32, UInt32 frames,
                           AudioBufferList *list) {
  Audio *a = ctx;

  if (enter(a)) {
    a->e.render(a->e.ctx, list->mBuffers[0].mData, frames);
    leave(a);
  }
  else {
    memset(list->mBuffers[0].mData, 0, frames * sizeof(float));
  }

  return noErr;
}

static bool set(AudioUnit unit, AudioUnitPropertyID property,
                AudioUnitScope scope, AudioUnitElement element,
                const void *value, UInt32 size) {
  return AudioUnitSetProperty(unit, property, scope, element, value, size) ==
         noErr;
}

static bool get(AudioObjectID object, AudioObjectPropertySelector selector,
                AudioObjectPropertyScope scope, void *value, UInt32 size) {
  AudioObjectPropertyAddress address = {selector, scope,
                                        kAudioObjectPropertyElementMain};

  return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size,
                                    value) == noErr;
}

static char *copy_string(AudioObjectID object,
                         AudioObjectPropertySelector selector) {
  CFStringRef string = nullptr;

  if (!get(object, selector, kAudioObjectPropertyScopeGlobal, &string,
           sizeof string) ||
      string == nullptr) {
    return nullptr;
  }

  char *copy = strdup(((__bridge NSString *)string).UTF8String ?: "");

  CFRelease(string);
  return copy;
}

// whether the device has channels on that side.
static bool has_channels(AudioDeviceID device, AudioObjectPropertyScope scope) {
  AudioObjectPropertyAddress address = {
    kAudioDevicePropertyStreamConfiguration, scope,
    kAudioObjectPropertyElementMain};
  UInt32 size = 0;

  if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) !=
        noErr ||
      size == 0) {
    return false;
  }

  AudioBufferList *list = malloc(size);
  UInt32 channels = 0;

  if (list != nullptr && AudioObjectGetPropertyData(device, &address, 0,
                                                    nullptr, &size, list) ==
                           noErr) {
    for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
      channels += list->mBuffers[i].mNumberChannels;
    }
  }

  free(list);
  return channels > 0;
}

static NSArray<AVCaptureDevice *> *cameras(void) {
  NSArray *types = @[
    AVCaptureDeviceTypeBuiltInWideAngleCamera,
    AVCaptureDeviceTypeExternal,
    AVCaptureDeviceTypeContinuityCamera,
    AVCaptureDeviceTypeDeskViewCamera,
  ];

  return [AVCaptureDeviceDiscoverySession
           discoverySessionWithDeviceTypes:types
                                 mediaType:AVMediaTypeVideo
                                  position:AVCaptureDevicePositionUnspecified]
    .devices;
}

static bool push(AudioDevice **list, size_t *count, char *id, char *name) {
  AudioDevice *grown = realloc(*list, (*count + 1) * sizeof *grown);

  if (grown == nullptr || id == nullptr || name == nullptr) {
    *list = grown != nullptr ? grown : *list;
    free(id);
    free(name);
    return false;
  }

  grown[(*count)++] = (AudioDevice){id, name};
  *list = grown;
  return true;
}

size_t audio_devices(AudioKind kind, AudioDevice **out) {
  size_t count = 0;

  *out = nullptr;

  if (kind == AUDIO_CAMERAS) {
    @autoreleasepool {
      for (AVCaptureDevice *camera in cameras()) {
        push(out, &count, strdup(camera.uniqueID.UTF8String ?: ""),
             strdup(camera.localizedName.UTF8String ?: ""));
      }
    }

    return count;
  }

  AudioObjectPropertyAddress address = {kAudioHardwarePropertyDevices,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
  UInt32 size = 0;

  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0,
                                     nullptr, &size) != noErr) {
    return 0;
  }

  AudioDeviceID *ids = malloc(size);

  if (ids == nullptr ||
      AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                 &size, ids) != noErr) {
    free(ids);
    return 0;
  }

  AudioObjectPropertyScope scope = kind == AUDIO_INPUTS
                                     ? kAudioObjectPropertyScopeInput
                                     : kAudioObjectPropertyScopeOutput;

  for (UInt32 i = 0; i < size / sizeof *ids; i++) {
    if (!has_channels(ids[i], scope)) {
      continue;
    }

    char *id = copy_string(ids[i], kAudioDevicePropertyDeviceUID);
    char *name = copy_string(ids[i], kAudioObjectPropertyName);

    // the voice processing unit makes devices of its own out of the ones
    // it uses, which are nobody's to pick.
    if (id != nullptr && name != nullptr &&
        (strncmp(id, "VPAUAggregate", 13) == 0 ||
         strncmp(id, "CADefaultDeviceAggregate", 24) == 0 ||
         strncmp(name, "VPAUAggregate", 13) == 0)) {
      free(id);
      free(name);
      continue;
    }

    push(out, &count, id, name);
  }

  free(ids);
  return count;
}

void audio_devices_free(AudioDevice *devices, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(devices[i].id);
    free(devices[i].name);
  }

  free(devices);
}

static uint32_t devices_version;
static void (*devices_changed)(void);

static void devices_moved(void) {
  devices_version++;

  if (devices_changed != nullptr) {
    devices_changed();
  }
}

uint32_t audio_devices_version(void (*changed)(void)) {
  static bool watching;

  if (watching) {
    return devices_version;
  }

  watching = true;
  devices_changed = changed;

  const AudioObjectPropertySelector watched[] = {
    kAudioHardwarePropertyDevices,
    kAudioHardwarePropertyDefaultInputDevice,
    kAudioHardwarePropertyDefaultOutputDevice,
  };

  for (size_t i = 0; i < sizeof watched / sizeof *watched; i++) {
    AudioObjectPropertyAddress address = {watched[i],
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};

    AudioObjectAddPropertyListenerBlock(
      kAudioObjectSystemObject, &address, dispatch_get_main_queue(),
      ^(UInt32, const AudioObjectPropertyAddress *) {
        devices_moved();
      });
  }

  for (NSNotificationName name in @[
         AVCaptureDeviceWasConnectedNotification,
         AVCaptureDeviceWasDisconnectedNotification,
       ]) {
    [NSNotificationCenter.defaultCenter
      addObserverForName:name
                  object:nil
                   queue:NSOperationQueue.mainQueue
              usingBlock:^(NSNotification *) {
                devices_moved();
              }];
  }

  return devices_version;
}

void audio_default_name(AudioKind kind, char *out, size_t cap) {
  out[0] = '\0';

  if (kind == AUDIO_CAMERAS) {
    @autoreleasepool {
      AVCaptureDevice *camera =
        [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];

      snprintf(out, cap, "%s", camera.localizedName.UTF8String ?: "");
    }

    return;
  }

  AudioDeviceID device = kAudioObjectUnknown;

  if (!get(kAudioObjectSystemObject,
           kind == AUDIO_INPUTS ? kAudioHardwarePropertyDefaultInputDevice
                                : kAudioHardwarePropertyDefaultOutputDevice,
           kAudioObjectPropertyScopeGlobal, &device, sizeof device) ||
      device == kAudioObjectUnknown) {
    return;
  }

  char *name = copy_string(device, kAudioObjectPropertyName);

  snprintf(out, cap, "%s", name != nullptr ? name : "");
  free(name);
}

// the device of an id, kAudioObjectUnknown when it is gone.
static AudioDeviceID device_of(const char *id) {
  if (id == nullptr || id[0] == '\0') {
    return kAudioObjectUnknown;
  }

  CFStringRef uid =
    CFStringCreateWithCString(nullptr, id, kCFStringEncodingUTF8);
  AudioDeviceID device = kAudioObjectUnknown;
  AudioObjectPropertyAddress address = {
    kAudioHardwarePropertyTranslateUIDToDevice,
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 size = sizeof device;

  if (uid == nullptr) {
    return kAudioObjectUnknown;
  }

  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                 sizeof uid, &uid, &size, &device) != noErr) {
    device = kAudioObjectUnknown;
  }

  CFRelease(uid);
  return device;
}

// on the queue of the units.
static bool open_unit(Audio *a) {
  // the unit that cancels the echo of what it plays out of what it hears.
  AudioComponentDescription description = {
    .componentType = kAudioUnitType_Output,
    .componentSubType = kAudioUnitSubType_VoiceProcessingIO,
    .componentManufacturer = kAudioUnitManufacturer_Apple,
  };
  AudioComponent component = AudioComponentFindNext(nullptr, &description);

  if (component == nullptr ||
      AudioComponentInstanceNew(component, &a->unit) != noErr) {
    a->unit = nullptr;
    return false;
  }

  AudioStreamBasicDescription format = {
    .mSampleRate = AUDIO_RATE,
    .mFormatID = kAudioFormatLinearPCM,
    .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                    kAudioFormatFlagIsNonInterleaved,
    .mBytesPerPacket = sizeof(float),
    .mFramesPerPacket = 1,
    .mBytesPerFrame = sizeof(float),
    .mChannelsPerFrame = 1,
    .mBitsPerChannel = 8 * sizeof(float),
  };
  UInt32 on = 1;
  AURenderCallbackStruct microphone = {on_microphone, a};
  AURenderCallbackStruct speaker = {on_speaker, a};

  bool ok =
    set(a->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
        MICROPHONE, &on, sizeof on) &&
    set(a->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
        MICROPHONE, &format, sizeof format) &&
    set(a->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
        SPEAKER, &format, sizeof format) &&
    set(a->unit, kAudioOutputUnitProperty_SetInputCallback,
        kAudioUnitScope_Global, MICROPHONE, &microphone, sizeof microphone) &&
    set(a->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
        SPEAKER, &speaker, sizeof speaker);

  // learned from chromium: the unit takes a device for each of its sides,
  // on the element of that side. it keeps the system's when not told.
  AudioDeviceID heard = device_of(a->input), played = device_of(a->output);

  if (heard != kAudioObjectUnknown) {
    set(a->unit, kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global, MICROPHONE, &heard, sizeof heard);
  }

  if (played != kAudioObjectUnknown) {
    set(a->unit, kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global, SPEAKER, &played, sizeof played);
  }

  // the unit lowers every other sound while it runs: as little as it can.
  AUVoiceIOOtherAudioDuckingConfiguration ducking = {
    .mEnableAdvancedDucking = false,
    .mDuckingLevel = kAUVoiceIOOtherAudioDuckingLevelMin,
  };

  set(a->unit, kAUVoiceIOProperty_OtherAudioDuckingConfiguration,
      kAudioUnitScope_Global, 0, &ducking, sizeof ducking);

  // the unit stays, for its devices, and hands the microphone through.
  UInt32 bypass = a->raw, gain = !a->raw;

  set(a->unit, kAUVoiceIOProperty_BypassVoiceProcessing,
      kAudioUnitScope_Global, 0, &bypass, sizeof bypass);
  set(a->unit, kAUVoiceIOProperty_VoiceProcessingEnableAGC,
      kAudioUnitScope_Global, 0, &gain, sizeof gain);

  ok = ok && AudioUnitInitialize(a->unit) == noErr;

  if (ok && AudioOutputUnitStart(a->unit) != noErr) {
    AudioUnitUninitialize(a->unit);
    ok = false;
  }

  if (!ok) {
    AudioComponentInstanceDispose(a->unit);
    a->unit = nullptr;
  }

  return ok;
}

Audio *audio_start(const AudioEvents *e, const char *input, const char *output,
                   bool raw, AudioResult *why) {
  *why = AUDIO_FAILED;

  if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] !=
      AVAuthorizationStatusAuthorized) {
    *why = AUDIO_DENIED;
    return nullptr;
  }

  Audio *a = calloc(1, sizeof *a);

  if (a == nullptr) {
    return nullptr;
  }

  a->e = *e;
  a->input = input != nullptr ? strdup(input) : nullptr;
  a->output = output != nullptr ? strdup(output) : nullptr;
  a->raw = raw;

  dispatch_async(units(), ^{
    if (open_unit(a)) {
      return;
    }

    // queued before what audio_stop queues to free it.
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!atomic_load(&a->stopped) && a->e.failed != nullptr) {
        a->e.failed(a->e.ctx);
      }
    });
  });

  *why = AUDIO_OK;
  return a;
}

bool audio_live(const Audio *a) {
  return atomic_load_explicit(&a->live, memory_order_relaxed);
}

void audio_stop(Audio *a) {
  if (a == nullptr) {
    return;
  }

  atomic_store(&a->stopped, true);

  // a callback that already started ends within a few milliseconds.
  while (atomic_load(&a->busy) != 0) {
    sched_yield();
  }

  dispatch_async(units(), ^{
    if (a->unit != nullptr) {
      AudioOutputUnitStop(a->unit);
      AudioUnitUninitialize(a->unit);
      AudioComponentInstanceDispose(a->unit);
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      free(a->input);
      free(a->output);
      free(a);
    });
  });
}

void audio_show_modes(void) {
  [AVCaptureDevice
    showSystemUserInterface:AVCaptureSystemUserInterfaceMicrophoneModes];
}
