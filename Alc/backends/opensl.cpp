/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* This is an OpenAL backend for Android using the native audio APIs based on
 * OpenSL ES 1.0.1. It is based on source code for the native-audio sample app
 * bundled with NDK.
 */

#include "config.h"

#include "backends/opensl.h"

#include <stdlib.h>
#include <jni.h>

#include <thread>

#include "alMain.h"
#include "alu.h"
#include "ringbuffer.h"
#include "threads.h"
#include "compat.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

/* Helper macros */
#define VCALL(obj, func)  ((*(obj))->func((obj), EXTRACT_VCALL_ARGS
#define VCALL0(obj, func)  ((*(obj))->func((obj) EXTRACT_VCALL_ARGS


static const ALCchar opensl_device[] = "OpenSL";


static SLuint32 GetChannelMask(enum DevFmtChannels chans)
{
    switch(chans)
    {
        case DevFmtMono: return SL_SPEAKER_FRONT_CENTER;
        case DevFmtStereo: return SL_SPEAKER_FRONT_LEFT|SL_SPEAKER_FRONT_RIGHT;
        case DevFmtQuad: return SL_SPEAKER_FRONT_LEFT|SL_SPEAKER_FRONT_RIGHT|
                                SL_SPEAKER_BACK_LEFT|SL_SPEAKER_BACK_RIGHT;
        case DevFmtX51: return SL_SPEAKER_FRONT_LEFT|SL_SPEAKER_FRONT_RIGHT|
                               SL_SPEAKER_FRONT_CENTER|SL_SPEAKER_LOW_FREQUENCY|
                               SL_SPEAKER_SIDE_LEFT|SL_SPEAKER_SIDE_RIGHT;
        case DevFmtX51Rear: return SL_SPEAKER_FRONT_LEFT|SL_SPEAKER_FRONT_RIGHT|
                                   SL_SPEAKER_FRONT_CENTER|SL_SPEAKER_LOW_FREQUENCY|
                                   SL_SPEAKER_BACK_LEFT|SL_SPEAKER_BACK_RIGHT;
        case DevFmtX61: return SL_SPEAKER_FRONT_LEFT|SL_SPEAKER_FRONT_RIGHT|
                               SL_SPEAKER_FRONT_CENTER|SL_SPEAKER_LOW_FREQUENCY|
                               SL_SPEAKER_BACK_CENTER|
                               SL_SPEAKER_SIDE_LEFT|SL_SPEAKER_SIDE_RIGHT;
        case DevFmtX71: return SL_SPEAKER_FRONT_LEFT|SL_SPEAKER_FRONT_RIGHT|
                               SL_SPEAKER_FRONT_CENTER|SL_SPEAKER_LOW_FREQUENCY|
                               SL_SPEAKER_BACK_LEFT|SL_SPEAKER_BACK_RIGHT|
                               SL_SPEAKER_SIDE_LEFT|SL_SPEAKER_SIDE_RIGHT;
        case DevFmtAmbi3D:
            break;
    }
    return 0;
}

#ifdef SL_DATAFORMAT_PCM_EX
static SLuint32 GetTypeRepresentation(enum DevFmtType type)
{
    switch(type)
    {
        case DevFmtUByte:
        case DevFmtUShort:
        case DevFmtUInt:
            return SL_PCM_REPRESENTATION_UNSIGNED_INT;
        case DevFmtByte:
        case DevFmtShort:
        case DevFmtInt:
            return SL_PCM_REPRESENTATION_SIGNED_INT;
        case DevFmtFloat:
            return SL_PCM_REPRESENTATION_FLOAT;
    }
    return 0;
}
#endif

static const char *res_str(SLresult result)
{
    switch(result)
    {
        case SL_RESULT_SUCCESS: return "Success";
        case SL_RESULT_PRECONDITIONS_VIOLATED: return "Preconditions violated";
        case SL_RESULT_PARAMETER_INVALID: return "Parameter invalid";
        case SL_RESULT_MEMORY_FAILURE: return "Memory failure";
        case SL_RESULT_RESOURCE_ERROR: return "Resource error";
        case SL_RESULT_RESOURCE_LOST: return "Resource lost";
        case SL_RESULT_IO_ERROR: return "I/O error";
        case SL_RESULT_BUFFER_INSUFFICIENT: return "Buffer insufficient";
        case SL_RESULT_CONTENT_CORRUPTED: return "Content corrupted";
        case SL_RESULT_CONTENT_UNSUPPORTED: return "Content unsupported";
        case SL_RESULT_CONTENT_NOT_FOUND: return "Content not found";
        case SL_RESULT_PERMISSION_DENIED: return "Permission denied";
        case SL_RESULT_FEATURE_UNSUPPORTED: return "Feature unsupported";
        case SL_RESULT_INTERNAL_ERROR: return "Internal error";
        case SL_RESULT_UNKNOWN_ERROR: return "Unknown error";
        case SL_RESULT_OPERATION_ABORTED: return "Operation aborted";
        case SL_RESULT_CONTROL_LOST: return "Control lost";
#ifdef SL_RESULT_READONLY
        case SL_RESULT_READONLY: return "ReadOnly";
#endif
#ifdef SL_RESULT_ENGINEOPTION_UNSUPPORTED
        case SL_RESULT_ENGINEOPTION_UNSUPPORTED: return "Engine option unsupported";
#endif
#ifdef SL_RESULT_SOURCE_SINK_INCOMPATIBLE
        case SL_RESULT_SOURCE_SINK_INCOMPATIBLE: return "Source/Sink incompatible";
#endif
    }
    return "Unknown error code";
}

#define PRINTERR(x, s) do {                                                      \
    if((x) != SL_RESULT_SUCCESS)                                                 \
        ERR("%s: %s\n", (s), res_str((x)));                                      \
} while(0)


struct ALCopenslPlayback final : public ALCbackend {
    /* engine interfaces */
    SLObjectItf mEngineObj{nullptr};
    SLEngineItf mEngine{nullptr};

    /* output mix interfaces */
    SLObjectItf mOutputMix{nullptr};

    /* buffer queue player interfaces */
    SLObjectItf mBufferQueueObj{nullptr};

    ll_ringbuffer_t *mRing{nullptr};
    al::semaphore mSem;

    ALsizei mFrameSize{0};

    std::atomic<ALenum> mKillNow{AL_TRUE};
    std::thread mThread;
};

static void ALCopenslPlayback_process(SLAndroidSimpleBufferQueueItf bq, void *context);
static int ALCopenslPlayback_mixerProc(ALCopenslPlayback *self);

static void ALCopenslPlayback_Construct(ALCopenslPlayback *self, ALCdevice *device);
static void ALCopenslPlayback_Destruct(ALCopenslPlayback *self);
static ALCenum ALCopenslPlayback_open(ALCopenslPlayback *self, const ALCchar *name);
static ALCboolean ALCopenslPlayback_reset(ALCopenslPlayback *self);
static ALCboolean ALCopenslPlayback_start(ALCopenslPlayback *self);
static void ALCopenslPlayback_stop(ALCopenslPlayback *self);
static DECLARE_FORWARD2(ALCopenslPlayback, ALCbackend, ALCenum, captureSamples, void*, ALCuint)
static DECLARE_FORWARD(ALCopenslPlayback, ALCbackend, ALCuint, availableSamples)
static ClockLatency ALCopenslPlayback_getClockLatency(ALCopenslPlayback *self);
static DECLARE_FORWARD(ALCopenslPlayback, ALCbackend, void, lock)
static DECLARE_FORWARD(ALCopenslPlayback, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCopenslPlayback)

DEFINE_ALCBACKEND_VTABLE(ALCopenslPlayback);


static void ALCopenslPlayback_Construct(ALCopenslPlayback *self, ALCdevice *device)
{
    new (self) ALCopenslPlayback{};
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    SET_VTABLE2(ALCopenslPlayback, ALCbackend, self);
}

static void ALCopenslPlayback_Destruct(ALCopenslPlayback* self)
{
    if(self->mBufferQueueObj != NULL)
        VCALL0(self->mBufferQueueObj,Destroy)();
    self->mBufferQueueObj = NULL;

    if(self->mOutputMix)
        VCALL0(self->mOutputMix,Destroy)();
    self->mOutputMix = NULL;

    if(self->mEngineObj)
        VCALL0(self->mEngineObj,Destroy)();
    self->mEngineObj = NULL;
    self->mEngine = NULL;

    ll_ringbuffer_free(self->mRing);
    self->mRing = NULL;

    ALCbackend_Destruct(STATIC_CAST(ALCbackend, self));
    self->~ALCopenslPlayback();
}


/* this callback handler is called every time a buffer finishes playing */
static void ALCopenslPlayback_process(SLAndroidSimpleBufferQueueItf UNUSED(bq), void *context)
{
    ALCopenslPlayback *self = static_cast<ALCopenslPlayback*>(context);

    /* A note on the ringbuffer usage: The buffer queue seems to hold on to the
     * pointer passed to the Enqueue method, rather than copying the audio.
     * Consequently, the ringbuffer contains the audio that is currently queued
     * and waiting to play. This process() callback is called when a buffer is
     * finished, so we simply move the read pointer up to indicate the space is
     * available for writing again, and wake up the mixer thread to mix and
     * queue more audio.
     */
    ll_ringbuffer_read_advance(self->mRing, 1);

    self->mSem.post();
}


static int ALCopenslPlayback_mixerProc(ALCopenslPlayback *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend,self)->mDevice;
    SLAndroidSimpleBufferQueueItf bufferQueue;
    SLPlayItf player;
    SLresult result;

    SetRTPriority();
    althrd_setname(MIXER_THREAD_NAME);

    result = VCALL(self->mBufferQueueObj,GetInterface)(SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                       &bufferQueue);
    PRINTERR(result, "bufferQueue->GetInterface SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mBufferQueueObj,GetInterface)(SL_IID_PLAY, &player);
        PRINTERR(result, "bufferQueue->GetInterface SL_IID_PLAY");
    }

    ALCopenslPlayback_lock(self);
    if(SL_RESULT_SUCCESS != result)
        aluHandleDisconnect(device, "Failed to get playback buffer: 0x%08x", result);

    while(SL_RESULT_SUCCESS == result && !self->mKillNow.load(std::memory_order_acquire) &&
          device->Connected.load(std::memory_order_acquire))
    {
        size_t todo;

        if(ll_ringbuffer_write_space(self->mRing) == 0)
        {
            SLuint32 state = 0;

            result = VCALL(player,GetPlayState)(&state);
            PRINTERR(result, "player->GetPlayState");
            if(SL_RESULT_SUCCESS == result && state != SL_PLAYSTATE_PLAYING)
            {
                result = VCALL(player,SetPlayState)(SL_PLAYSTATE_PLAYING);
                PRINTERR(result, "player->SetPlayState");
            }
            if(SL_RESULT_SUCCESS != result)
            {
                aluHandleDisconnect(device, "Failed to start platback: 0x%08x", result);
                break;
            }

            if(ll_ringbuffer_write_space(self->mRing) == 0)
            {
                ALCopenslPlayback_unlock(self);
                self->mSem.wait();
                ALCopenslPlayback_lock(self);
                continue;
            }
        }

        auto data = ll_ringbuffer_get_write_vector(self->mRing);
        aluMixData(device, data.first.buf, data.first.len*device->UpdateSize);
        if(data.second.len > 0)
            aluMixData(device, data.second.buf, data.second.len*device->UpdateSize);

        todo = data.first.len+data.second.len;
        ll_ringbuffer_write_advance(self->mRing, todo);

        for(size_t i = 0;i < todo;i++)
        {
            if(!data.first.len)
            {
                data.first = data.second;
                data.second.buf = nullptr;
                data.second.len = 0;
            }

            result = VCALL(bufferQueue,Enqueue)(data.first.buf,
                device->UpdateSize*self->mFrameSize);
            PRINTERR(result, "bufferQueue->Enqueue");
            if(SL_RESULT_SUCCESS != result)
            {
                aluHandleDisconnect(device, "Failed to queue audio: 0x%08x", result);
                break;
            }

            data.first.len--;
            data.first.buf += device->UpdateSize*self->mFrameSize;
        }
    }
    ALCopenslPlayback_unlock(self);

    return 0;
}


static ALCenum ALCopenslPlayback_open(ALCopenslPlayback *self, const ALCchar *name)
{
    ALCdevice *device = STATIC_CAST(ALCbackend,self)->mDevice;
    SLresult result;

    if(!name)
        name = opensl_device;
    else if(strcmp(name, opensl_device) != 0)
        return ALC_INVALID_VALUE;

    // create engine
    result = slCreateEngine(&self->mEngineObj, 0, NULL, 0, NULL, NULL);
    PRINTERR(result, "slCreateEngine");
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mEngineObj,Realize)(SL_BOOLEAN_FALSE);
        PRINTERR(result, "engine->Realize");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mEngineObj,GetInterface)(SL_IID_ENGINE, &self->mEngine);
        PRINTERR(result, "engine->GetInterface");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mEngine,CreateOutputMix)(&self->mOutputMix, 0, NULL, NULL);
        PRINTERR(result, "engine->CreateOutputMix");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mOutputMix,Realize)(SL_BOOLEAN_FALSE);
        PRINTERR(result, "outputMix->Realize");
    }

    if(SL_RESULT_SUCCESS != result)
    {
        if(self->mOutputMix != NULL)
            VCALL0(self->mOutputMix,Destroy)();
        self->mOutputMix = NULL;

        if(self->mEngineObj != NULL)
            VCALL0(self->mEngineObj,Destroy)();
        self->mEngineObj = NULL;
        self->mEngine = NULL;

        return ALC_INVALID_VALUE;
    }

    device->DeviceName = name;
    return ALC_NO_ERROR;
}

static ALCboolean ALCopenslPlayback_reset(ALCopenslPlayback *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend,self)->mDevice;
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq;
    SLDataLocator_OutputMix loc_outmix;
    SLDataSource audioSrc;
    SLDataSink audioSnk;
    ALuint sampleRate;
    SLInterfaceID ids[2];
    SLboolean reqs[2];
    SLresult result;

    if(self->mBufferQueueObj != NULL)
        VCALL0(self->mBufferQueueObj,Destroy)();
    self->mBufferQueueObj = NULL;

    ll_ringbuffer_free(self->mRing);
    self->mRing = NULL;

    sampleRate = device->Frequency;
#if 0
    if(!(device->Flags&DEVICE_FREQUENCY_REQUEST))
    {
        /* FIXME: Disabled until I figure out how to get the Context needed for
         * the getSystemService call.
         */
        JNIEnv *env = Android_GetJNIEnv();
        jobject jctx = Android_GetContext();

        /* Get necessary stuff for using java.lang.Integer,
         * android.content.Context, and android.media.AudioManager.
         */
        jclass int_cls = JCALL(env,FindClass)("java/lang/Integer");
        jmethodID int_parseint = JCALL(env,GetStaticMethodID)(int_cls,
            "parseInt", "(Ljava/lang/String;)I"
        );
        TRACE("Integer: %p, parseInt: %p\n", int_cls, int_parseint);

        jclass ctx_cls = JCALL(env,FindClass)("android/content/Context");
        jfieldID ctx_audsvc = JCALL(env,GetStaticFieldID)(ctx_cls,
            "AUDIO_SERVICE", "Ljava/lang/String;"
        );
        jmethodID ctx_getSysSvc = JCALL(env,GetMethodID)(ctx_cls,
            "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"
        );
        TRACE("Context: %p, AUDIO_SERVICE: %p, getSystemService: %p\n",
              ctx_cls, ctx_audsvc, ctx_getSysSvc);

        jclass audmgr_cls = JCALL(env,FindClass)("android/media/AudioManager");
        jfieldID audmgr_prop_out_srate = JCALL(env,GetStaticFieldID)(audmgr_cls,
            "PROPERTY_OUTPUT_SAMPLE_RATE", "Ljava/lang/String;"
        );
        jmethodID audmgr_getproperty = JCALL(env,GetMethodID)(audmgr_cls,
            "getProperty", "(Ljava/lang/String;)Ljava/lang/String;"
        );
        TRACE("AudioManager: %p, PROPERTY_OUTPUT_SAMPLE_RATE: %p, getProperty: %p\n",
              audmgr_cls, audmgr_prop_out_srate, audmgr_getproperty);

        const char *strchars;
        jstring strobj;

        /* Now make the calls. */
        //AudioManager audMgr = (AudioManager)getSystemService(Context.AUDIO_SERVICE);
        strobj = JCALL(env,GetStaticObjectField)(ctx_cls, ctx_audsvc);
        jobject audMgr = JCALL(env,CallObjectMethod)(jctx, ctx_getSysSvc, strobj);
        strchars = JCALL(env,GetStringUTFChars)(strobj, NULL);
        TRACE("Context.getSystemService(%s) = %p\n", strchars, audMgr);
        JCALL(env,ReleaseStringUTFChars)(strobj, strchars);

        //String srateStr = audMgr.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE);
        strobj = JCALL(env,GetStaticObjectField)(audmgr_cls, audmgr_prop_out_srate);
        jstring srateStr = JCALL(env,CallObjectMethod)(audMgr, audmgr_getproperty, strobj);
        strchars = JCALL(env,GetStringUTFChars)(strobj, NULL);
        TRACE("audMgr.getProperty(%s) = %p\n", strchars, srateStr);
        JCALL(env,ReleaseStringUTFChars)(strobj, strchars);

        //int sampleRate = Integer.parseInt(srateStr);
        sampleRate = JCALL(env,CallStaticIntMethod)(int_cls, int_parseint, srateStr);

        strchars = JCALL(env,GetStringUTFChars)(srateStr, NULL);
        TRACE("Got system sample rate %uhz (%s)\n", sampleRate, strchars);
        JCALL(env,ReleaseStringUTFChars)(srateStr, strchars);

        if(!sampleRate) sampleRate = device->Frequency;
        else sampleRate = maxu(sampleRate, MIN_OUTPUT_RATE);
    }
#endif

    if(sampleRate != device->Frequency)
    {
        device->NumUpdates = (device->NumUpdates*sampleRate + (device->Frequency>>1)) /
                             device->Frequency;
        device->NumUpdates = maxu(device->NumUpdates, 2);
        device->Frequency = sampleRate;
    }

    device->FmtChans = DevFmtStereo;
    device->FmtType = DevFmtShort;

    SetDefaultWFXChannelOrder(device);
    self->mFrameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->mAmbiOrder);


    loc_bufq.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
    loc_bufq.numBuffers = device->NumUpdates;

#ifdef SL_DATAFORMAT_PCM_EX
    SLDataFormat_PCM_EX format_pcm;
    format_pcm.formatType = SL_DATAFORMAT_PCM_EX;
    format_pcm.numChannels = ChannelsFromDevFmt(device->FmtChans, device->mAmbiOrder);
    format_pcm.sampleRate = device->Frequency * 1000;
    format_pcm.bitsPerSample = BytesFromDevFmt(device->FmtType) * 8;
    format_pcm.containerSize = format_pcm.bitsPerSample;
    format_pcm.channelMask = GetChannelMask(device->FmtChans);
    format_pcm.endianness = IS_LITTLE_ENDIAN ? SL_BYTEORDER_LITTLEENDIAN :
                                               SL_BYTEORDER_BIGENDIAN;
    format_pcm.representation = GetTypeRepresentation(device->FmtType);
#else
    SLDataFormat_PCM format_pcm;
    format_pcm.formatType = SL_DATAFORMAT_PCM;
    format_pcm.numChannels = ChannelsFromDevFmt(device->FmtChans, device->mAmbiOrder);
    format_pcm.samplesPerSec = device->Frequency * 1000;
    format_pcm.bitsPerSample = BytesFromDevFmt(device->FmtType) * 8;
    format_pcm.containerSize = format_pcm.bitsPerSample;
    format_pcm.channelMask = GetChannelMask(device->FmtChans);
    format_pcm.endianness = IS_LITTLE_ENDIAN ? SL_BYTEORDER_LITTLEENDIAN :
                                               SL_BYTEORDER_BIGENDIAN;
#endif

    audioSrc.pLocator = &loc_bufq;
    audioSrc.pFormat = &format_pcm;

    loc_outmix.locatorType = SL_DATALOCATOR_OUTPUTMIX;
    loc_outmix.outputMix = self->mOutputMix;
    audioSnk.pLocator = &loc_outmix;
    audioSnk.pFormat = NULL;


    ids[0]  = SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    reqs[0] = SL_BOOLEAN_TRUE;
    ids[1]  = SL_IID_ANDROIDCONFIGURATION;
    reqs[1] = SL_BOOLEAN_FALSE;

    result = VCALL(self->mEngine,CreateAudioPlayer)(&self->mBufferQueueObj,
        &audioSrc, &audioSnk, COUNTOF(ids), ids, reqs
    );
    PRINTERR(result, "engine->CreateAudioPlayer");
    if(SL_RESULT_SUCCESS == result)
    {
        /* Set the stream type to "media" (games, music, etc), if possible. */
        SLAndroidConfigurationItf config;
        result = VCALL(self->mBufferQueueObj,GetInterface)(SL_IID_ANDROIDCONFIGURATION, &config);
        PRINTERR(result, "bufferQueue->GetInterface SL_IID_ANDROIDCONFIGURATION");
        if(SL_RESULT_SUCCESS == result)
        {
            SLint32 streamType = SL_ANDROID_STREAM_MEDIA;
            result = VCALL(config,SetConfiguration)(SL_ANDROID_KEY_STREAM_TYPE,
                &streamType, sizeof(streamType)
            );
            PRINTERR(result, "config->SetConfiguration");
        }

        /* Clear any error since this was optional. */
        result = SL_RESULT_SUCCESS;
    }
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mBufferQueueObj,Realize)(SL_BOOLEAN_FALSE);
        PRINTERR(result, "bufferQueue->Realize");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        self->mRing = ll_ringbuffer_create(device->NumUpdates,
            self->mFrameSize*device->UpdateSize, true
        );
        if(!self->mRing)
        {
            ERR("Out of memory allocating ring buffer %ux%u %u\n", device->UpdateSize,
                device->NumUpdates, self->mFrameSize);
            result = SL_RESULT_MEMORY_FAILURE;
        }
    }

    if(SL_RESULT_SUCCESS != result)
    {
        if(self->mBufferQueueObj != NULL)
            VCALL0(self->mBufferQueueObj,Destroy)();
        self->mBufferQueueObj = NULL;

        return ALC_FALSE;
    }

    return ALC_TRUE;
}

static ALCboolean ALCopenslPlayback_start(ALCopenslPlayback *self)
{
    SLAndroidSimpleBufferQueueItf bufferQueue;
    SLresult result;

    ll_ringbuffer_reset(self->mRing);

    result = VCALL(self->mBufferQueueObj,GetInterface)(SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                       &bufferQueue);
    PRINTERR(result, "bufferQueue->GetInterface");
    if(SL_RESULT_SUCCESS != result)
        return ALC_FALSE;

    result = VCALL(bufferQueue,RegisterCallback)(ALCopenslPlayback_process, self);
    PRINTERR(result, "bufferQueue->RegisterCallback");
    if(SL_RESULT_SUCCESS != result)
        return ALC_FALSE;

    try {
        self->mKillNow.store(AL_FALSE);
        self->mThread = std::thread(ALCopenslPlayback_mixerProc, self);
        return ALC_TRUE;
    }
    catch(std::exception& e) {
        ERR("Could not create playback thread: %s\n", e.what());
    }
    catch(...) {
    }
    return ALC_FALSE;
}


static void ALCopenslPlayback_stop(ALCopenslPlayback *self)
{
    SLAndroidSimpleBufferQueueItf bufferQueue;
    SLPlayItf player;
    SLresult result;

    if(self->mKillNow.exchange(AL_TRUE) || !self->mThread.joinable())
        return;

    self->mSem.post();
    self->mThread.join();

    result = VCALL(self->mBufferQueueObj,GetInterface)(SL_IID_PLAY, &player);
    PRINTERR(result, "bufferQueue->GetInterface");
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(player,SetPlayState)(SL_PLAYSTATE_STOPPED);
        PRINTERR(result, "player->SetPlayState");
    }

    result = VCALL(self->mBufferQueueObj,GetInterface)(SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                       &bufferQueue);
    PRINTERR(result, "bufferQueue->GetInterface");
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL0(bufferQueue,Clear)();
        PRINTERR(result, "bufferQueue->Clear");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(bufferQueue,RegisterCallback)(NULL, NULL);
        PRINTERR(result, "bufferQueue->RegisterCallback");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        SLAndroidSimpleBufferQueueState state;
        do {
            std::this_thread::yield();
            result = VCALL(bufferQueue,GetState)(&state);
        } while(SL_RESULT_SUCCESS == result && state.count > 0);
        PRINTERR(result, "bufferQueue->GetState");
    }
}

static ClockLatency ALCopenslPlayback_getClockLatency(ALCopenslPlayback *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    ClockLatency ret;

    ALCopenslPlayback_lock(self);
    ret.ClockTime = GetDeviceClockTime(device);
    ret.Latency  = std::chrono::seconds{ll_ringbuffer_read_space(self->mRing)*device->UpdateSize};
    ret.Latency /= device->Frequency;
    ALCopenslPlayback_unlock(self);

    return ret;
}


struct ALCopenslCapture final : public ALCbackend {
    /* engine interfaces */
    SLObjectItf mEngineObj{nullptr};
    SLEngineItf mEngine;

    /* recording interfaces */
    SLObjectItf mRecordObj{nullptr};

    ll_ringbuffer_t *mRing{nullptr};
    ALCuint mSplOffset{0u};

    ALsizei mFrameSize{0};
};

static void ALCopenslCapture_process(SLAndroidSimpleBufferQueueItf bq, void *context);

static void ALCopenslCapture_Construct(ALCopenslCapture *self, ALCdevice *device);
static void ALCopenslCapture_Destruct(ALCopenslCapture *self);
static ALCenum ALCopenslCapture_open(ALCopenslCapture *self, const ALCchar *name);
static DECLARE_FORWARD(ALCopenslCapture, ALCbackend, ALCboolean, reset)
static ALCboolean ALCopenslCapture_start(ALCopenslCapture *self);
static void ALCopenslCapture_stop(ALCopenslCapture *self);
static ALCenum ALCopenslCapture_captureSamples(ALCopenslCapture *self, ALCvoid *buffer, ALCuint samples);
static ALCuint ALCopenslCapture_availableSamples(ALCopenslCapture *self);
static DECLARE_FORWARD(ALCopenslCapture, ALCbackend, ClockLatency, getClockLatency)
static DECLARE_FORWARD(ALCopenslCapture, ALCbackend, void, lock)
static DECLARE_FORWARD(ALCopenslCapture, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCopenslCapture)
DEFINE_ALCBACKEND_VTABLE(ALCopenslCapture);


static void ALCopenslCapture_Construct(ALCopenslCapture *self, ALCdevice *device)
{
    new (self) ALCopenslCapture{};
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    SET_VTABLE2(ALCopenslCapture, ALCbackend, self);
}

static void ALCopenslCapture_Destruct(ALCopenslCapture *self)
{
    if(self->mRecordObj != NULL)
        VCALL0(self->mRecordObj,Destroy)();
    self->mRecordObj = NULL;

    if(self->mEngineObj != NULL)
        VCALL0(self->mEngineObj,Destroy)();
    self->mEngineObj = NULL;
    self->mEngine = NULL;

    ll_ringbuffer_free(self->mRing);
    self->mRing = NULL;

    ALCbackend_Destruct(STATIC_CAST(ALCbackend, self));
    self->~ALCopenslCapture();
}


static void ALCopenslCapture_process(SLAndroidSimpleBufferQueueItf UNUSED(bq), void *context)
{
    ALCopenslCapture *self = static_cast<ALCopenslCapture*>(context);
    /* A new chunk has been written into the ring buffer, advance it. */
    ll_ringbuffer_write_advance(self->mRing, 1);
}


static ALCenum ALCopenslCapture_open(ALCopenslCapture *self, const ALCchar *name)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    SLDataLocator_AndroidSimpleBufferQueue loc_bq;
    SLAndroidSimpleBufferQueueItf bufferQueue;
    SLDataLocator_IODevice loc_dev;
    SLDataSource audioSrc;
    SLDataSink audioSnk;
    SLresult result;

    if(!name)
        name = opensl_device;
    else if(strcmp(name, opensl_device) != 0)
        return ALC_INVALID_VALUE;

    result = slCreateEngine(&self->mEngineObj, 0, NULL, 0, NULL, NULL);
    PRINTERR(result, "slCreateEngine");
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mEngineObj,Realize)(SL_BOOLEAN_FALSE);
        PRINTERR(result, "engine->Realize");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mEngineObj,GetInterface)(SL_IID_ENGINE, &self->mEngine);
        PRINTERR(result, "engine->GetInterface");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        /* Ensure the total length is at least 100ms */
        ALsizei length = maxi(device->NumUpdates * device->UpdateSize,
                              device->Frequency / 10);
        /* Ensure the per-chunk length is at least 10ms, and no more than 50ms. */
        ALsizei update_len = clampi(device->NumUpdates*device->UpdateSize / 3,
                                    device->Frequency / 100,
                                    device->Frequency / 100 * 5);

        device->UpdateSize = update_len;
        device->NumUpdates = (length+update_len-1) / update_len;

        self->mFrameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->mAmbiOrder);
    }
    loc_dev.locatorType = SL_DATALOCATOR_IODEVICE;
    loc_dev.deviceType = SL_IODEVICE_AUDIOINPUT;
    loc_dev.deviceID = SL_DEFAULTDEVICEID_AUDIOINPUT;
    loc_dev.device = NULL;

    audioSrc.pLocator = &loc_dev;
    audioSrc.pFormat = NULL;

    loc_bq.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
    loc_bq.numBuffers = device->NumUpdates;

#ifdef SL_DATAFORMAT_PCM_EX
    SLDataFormat_PCM_EX format_pcm;
    format_pcm.formatType = SL_DATAFORMAT_PCM_EX;
    format_pcm.numChannels = ChannelsFromDevFmt(device->FmtChans, device->mAmbiOrder);
    format_pcm.sampleRate = device->Frequency * 1000;
    format_pcm.bitsPerSample = BytesFromDevFmt(device->FmtType) * 8;
    format_pcm.containerSize = format_pcm.bitsPerSample;
    format_pcm.channelMask = GetChannelMask(device->FmtChans);
    format_pcm.endianness = IS_LITTLE_ENDIAN ? SL_BYTEORDER_LITTLEENDIAN :
                                               SL_BYTEORDER_BIGENDIAN;
    format_pcm.representation = GetTypeRepresentation(device->FmtType);
#else
    SLDataFormat_PCM format_pcm;
    format_pcm.formatType = SL_DATAFORMAT_PCM;
    format_pcm.numChannels = ChannelsFromDevFmt(device->FmtChans, device->mAmbiOrder);
    format_pcm.samplesPerSec = device->Frequency * 1000;
    format_pcm.bitsPerSample = BytesFromDevFmt(device->FmtType) * 8;
    format_pcm.containerSize = format_pcm.bitsPerSample;
    format_pcm.channelMask = GetChannelMask(device->FmtChans);
    format_pcm.endianness = IS_LITTLE_ENDIAN ? SL_BYTEORDER_LITTLEENDIAN :
                                               SL_BYTEORDER_BIGENDIAN;
#endif

    audioSnk.pLocator = &loc_bq;
    audioSnk.pFormat = &format_pcm;

    if(SL_RESULT_SUCCESS == result)
    {
        const SLInterfaceID ids[2] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
        const SLboolean reqs[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE };

        result = VCALL(self->mEngine,CreateAudioRecorder)(&self->mRecordObj,
            &audioSrc, &audioSnk, COUNTOF(ids), ids, reqs
        );
        PRINTERR(result, "engine->CreateAudioRecorder");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        /* Set the record preset to "generic", if possible. */
        SLAndroidConfigurationItf config;
        result = VCALL(self->mRecordObj,GetInterface)(SL_IID_ANDROIDCONFIGURATION, &config);
        PRINTERR(result, "recordObj->GetInterface SL_IID_ANDROIDCONFIGURATION");
        if(SL_RESULT_SUCCESS == result)
        {
            SLuint32 preset = SL_ANDROID_RECORDING_PRESET_GENERIC;
            result = VCALL(config,SetConfiguration)(SL_ANDROID_KEY_RECORDING_PRESET,
                &preset, sizeof(preset)
            );
            PRINTERR(result, "config->SetConfiguration");
        }

        /* Clear any error since this was optional. */
        result = SL_RESULT_SUCCESS;
    }
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(self->mRecordObj,Realize)(SL_BOOLEAN_FALSE);
        PRINTERR(result, "recordObj->Realize");
    }

    if(SL_RESULT_SUCCESS == result)
    {
        self->mRing = ll_ringbuffer_create(device->NumUpdates,
            device->UpdateSize*self->mFrameSize, false
        );

        result = VCALL(self->mRecordObj,GetInterface)(SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                      &bufferQueue);
        PRINTERR(result, "recordObj->GetInterface");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(bufferQueue,RegisterCallback)(ALCopenslCapture_process, self);
        PRINTERR(result, "bufferQueue->RegisterCallback");
    }
    if(SL_RESULT_SUCCESS == result)
    {
        ALsizei chunk_size = device->UpdateSize * self->mFrameSize;
        size_t i;

        auto data = ll_ringbuffer_get_write_vector(self->mRing);
        for(i = 0;i < data.first.len && SL_RESULT_SUCCESS == result;i++)
        {
            result = VCALL(bufferQueue,Enqueue)(data.first.buf + chunk_size*i, chunk_size);
            PRINTERR(result, "bufferQueue->Enqueue");
        }
        for(i = 0;i < data.second.len && SL_RESULT_SUCCESS == result;i++)
        {
            result = VCALL(bufferQueue,Enqueue)(data.second.buf + chunk_size*i, chunk_size);
            PRINTERR(result, "bufferQueue->Enqueue");
        }
    }

    if(SL_RESULT_SUCCESS != result)
    {
        if(self->mRecordObj != NULL)
            VCALL0(self->mRecordObj,Destroy)();
        self->mRecordObj = NULL;

        if(self->mEngineObj != NULL)
            VCALL0(self->mEngineObj,Destroy)();
        self->mEngineObj = NULL;
        self->mEngine = NULL;

        return ALC_INVALID_VALUE;
    }

    device->DeviceName = name;
    return ALC_NO_ERROR;
}

static ALCboolean ALCopenslCapture_start(ALCopenslCapture *self)
{
    SLRecordItf record;
    SLresult result;

    result = VCALL(self->mRecordObj,GetInterface)(SL_IID_RECORD, &record);
    PRINTERR(result, "recordObj->GetInterface");

    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(record,SetRecordState)(SL_RECORDSTATE_RECORDING);
        PRINTERR(result, "record->SetRecordState");
    }

    if(SL_RESULT_SUCCESS != result)
    {
        ALCopenslCapture_lock(self);
        aluHandleDisconnect(STATIC_CAST(ALCbackend, self)->mDevice,
                            "Failed to start capture: 0x%08x", result);
        ALCopenslCapture_unlock(self);
        return ALC_FALSE;
    }

    return ALC_TRUE;
}

static void ALCopenslCapture_stop(ALCopenslCapture *self)
{
    SLRecordItf record;
    SLresult result;

    result = VCALL(self->mRecordObj,GetInterface)(SL_IID_RECORD, &record);
    PRINTERR(result, "recordObj->GetInterface");

    if(SL_RESULT_SUCCESS == result)
    {
        result = VCALL(record,SetRecordState)(SL_RECORDSTATE_PAUSED);
        PRINTERR(result, "record->SetRecordState");
    }
}

static ALCenum ALCopenslCapture_captureSamples(ALCopenslCapture *self, ALCvoid *buffer, ALCuint samples)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    ALsizei chunk_size = device->UpdateSize * self->mFrameSize;
    SLAndroidSimpleBufferQueueItf bufferQueue;
    SLresult result;
    ALCuint i;

    result = VCALL(self->mRecordObj,GetInterface)(SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                  &bufferQueue);
    PRINTERR(result, "recordObj->GetInterface");

    /* Read the desired samples from the ring buffer then advance its read
     * pointer.
     */
    auto data = ll_ringbuffer_get_read_vector(self->mRing);
    for(i = 0;i < samples;)
    {
        ALCuint rem = minu(samples - i, device->UpdateSize - self->mSplOffset);
        memcpy((ALCbyte*)buffer + i*self->mFrameSize,
               data.first.buf + self->mSplOffset*self->mFrameSize,
               rem * self->mFrameSize);

        self->mSplOffset += rem;
        if(self->mSplOffset == device->UpdateSize)
        {
            /* Finished a chunk, reset the offset and advance the read pointer. */
            self->mSplOffset = 0;

            ll_ringbuffer_read_advance(self->mRing, 1);
            result = VCALL(bufferQueue,Enqueue)(data.first.buf, chunk_size);
            PRINTERR(result, "bufferQueue->Enqueue");
            if(SL_RESULT_SUCCESS != result) break;

            data.first.len--;
            if(!data.first.len)
                data.first = data.second;
            else
                data.first.buf += chunk_size;
        }

        i += rem;
    }

    if(SL_RESULT_SUCCESS != result)
    {
        ALCopenslCapture_lock(self);
        aluHandleDisconnect(device, "Failed to update capture buffer: 0x%08x", result);
        ALCopenslCapture_unlock(self);
        return ALC_INVALID_DEVICE;
    }

    return ALC_NO_ERROR;
}

static ALCuint ALCopenslCapture_availableSamples(ALCopenslCapture *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    return ll_ringbuffer_read_space(self->mRing) * device->UpdateSize;
}


bool OSLBackendFactory::init() { return true; }

bool OSLBackendFactory::querySupport(ALCbackend_Type type)
{ return (type == ALCbackend_Playback || type == ALCbackend_Capture); }

void OSLBackendFactory::probe(enum DevProbe type, std::string *outnames)
{
    switch(type)
    {
        case ALL_DEVICE_PROBE:
        case CAPTURE_DEVICE_PROBE:
            /* Includes null char. */
            outnames->append(opensl_device, sizeof(opensl_device));
            break;
    }
}

ALCbackend *OSLBackendFactory::createBackend(ALCdevice *device, ALCbackend_Type type)
{
    if(type == ALCbackend_Playback)
    {
        ALCopenslPlayback *backend;
        NEW_OBJ(backend, ALCopenslPlayback)(device);
        if(!backend) return nullptr;
        return STATIC_CAST(ALCbackend, backend);
    }
    if(type == ALCbackend_Capture)
    {
        ALCopenslCapture *backend;
        NEW_OBJ(backend, ALCopenslCapture)(device);
        if(!backend) return nullptr;
        return STATIC_CAST(ALCbackend, backend);
    }

    return nullptr;
}

BackendFactory &OSLBackendFactory::getFactory()
{
    static OSLBackendFactory factory{};
    return factory;
}
