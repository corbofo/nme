#include "Audio.h"
#include <Sound.h>
#include <Display.h>
#include <SDL.h>
#include <SDL_mixer.h>
#include <Sound.h>
#include <hx/Thread.h>



namespace nme
{

SDLAudioState gSDLAudioState = sdaNotInit;

class SDLSoundChannel;

bool sChannelsInit = false;
enum { sMaxChannels = 8 };

bool sUsedChannel[sMaxChannels];
bool sDoneChannel[sMaxChannels];
void *sUsedMusic = 0;
bool sDoneMusic = false;
enum { STEREO_SAMPLES = 2 };

unsigned int  sSoundPos = 0;
double        sLastMusicUpdate = 0;
double        sMusicFrequency = 0;

void onChannelDone(int inChannel)
{
   if (sUsedChannel[inChannel])
      sDoneChannel[inChannel] = true;
}

void onMusicDone()
{
   if (sUsedMusic)
      sDoneMusic = true;
}

/*
extern "C" void music_mixer(void *udata, Uint8 *stream, int len);
void onMusic(void *udata, Uint8 *stream, int len)
{
   music_mixer(Mix_GetMusicHookData(), stream, len);
}
*/

void  onPostMix(void *udata, Uint8 *stream, int len)
{
   sSoundPos += len / sizeof(short) / STEREO_SAMPLES ;
   sLastMusicUpdate = GetTimeStamp();
}

int getMixerTime(int inTime0)
{
   double now = GetTimeStamp();
   if (now>sLastMusicUpdate+1)
      return sSoundPos;

   return (sSoundPos-inTime0) + (int)( (now - sLastMusicUpdate)*sMusicFrequency );
}


static bool Init()
{
   if (gSDLAudioState==sdaNotInit)
   {
      ELOG("Please init Stage before creating sound.");
      return false;
   }
   if (gSDLAudioState!=sdaOpen)
     return false;


   if (!sChannelsInit)
   {
      sChannelsInit = true;
      for(int i=0;i<sMaxChannels;i++)
      {
         sUsedChannel[i] = false;
         sDoneChannel[i] = false;
      }
      Mix_ChannelFinished(onChannelDone);
      Mix_HookMusicFinished(onMusicDone);
      //Mix_HookMusic(onMusic,0);
      #ifndef EMSCRIPTEN
      Mix_SetPostMix(onPostMix,0);
      #endif
   }

   return sChannelsInit;
}

// ---  Using "Mix_Chunk" API ----------------------------------------------------


class SDLSoundChannel : public SoundChannel
{
  enum { BUF_SIZE = (1<<17) };

public:
   SDLSoundChannel(Object *inSound, Mix_Chunk *inChunk, double inStartTime, int inLoops,
                  const SoundTransform &inTransform)
   {
      initSpec();
      mChunk = inChunk;
      mDynamicBuffer = 0;
      mSound = inSound;
      mSound->IncRef();
      startSample = endSample = sSoundPos;
      playing = true; 
      loopsPending = 0;

      mChannel = -1;

      bool valid = false;

      mOffsetChunk.alen = 0;
      if (mFrequency && mChunk && mChunk->alen)
      {
         valid = true;
         mOffsetChunk = *mChunk;
         mOffsetChunk.allocated = 0;
         int startBytes = (int)(inStartTime*0.001*mFrequency*sizeof(short)*STEREO_SAMPLES) & ~3;
         int startLoops = startBytes / mChunk->alen;
         if (inLoops>=0)
         {
            inLoops-=startLoops; 
            if (inLoops<0)
               valid = false;
         }
         startBytes = startBytes % mChunk->alen;
         if (valid)
         {
            startSample -= startBytes/(sizeof(short)*STEREO_SAMPLES);
            endSample = startSample;
            mOffsetChunk.alen -= startBytes;
            mOffsetChunk.abuf += startBytes;
            if (startBytes)
            {
               loopsPending = inLoops;
               inLoops = 0;
            }
         }
      }



      // Allocate myself a channel
      if (valid)
      {
         for(int i=0;i<sMaxChannels;i++)
            if (!sUsedChannel[i])
            {
               IncRef();
               sDoneChannel[i] = false;
               sUsedChannel[i] = true;
               mChannel = i;
               break;
            }
      }


      if (mChannel<0 || Mix_PlayChannel( mChannel , &mOffsetChunk, inLoops<0 ? -1 : inLoops==0 ? 0 : inLoops-1 )<0)
      {
         onChannelDone(mChannel);
      }
      else
      {
         setTransform(inTransform);
     }
   }

   void setTransform(const SoundTransform &inTransform) 
   {
      if (mChannel>=0)
      {
         Mix_Volume( mChannel, inTransform.volume*MIX_MAX_VOLUME );

         int left = (1-inTransform.pan)*255;
         if (left<0) left = 0;
         if (left>255) left = 255;
   
         int right = (inTransform.pan + 1)*255;
         if (right<0) right = 0;
         if (right>255) right = 255;

         Mix_SetPanning( mChannel, left, right );
      }
   }


   void initSpec()
   {
      Mix_QuerySpec(&mFrequency, &mFormat, &mChannels);
      if (mFrequency!=44100)
         ELOG("Warning - Frequency mismatch %d",mFrequency);
      if (mFormat!=32784)
         ELOG("Warning - Format mismatch    %d",mFormat);
      if (mChannels!=2)
         ELOG("Warning - channe mismatch    %d",mChannels);

      if (sMusicFrequency==0)
         sMusicFrequency = mFrequency;
   }

   SDLSoundChannel(const ByteArray &inBytes, const SoundTransform &inTransform)
   {
      initSpec();
      mChunk = 0;
      mDynamicBuffer = new short[BUF_SIZE * STEREO_SAMPLES];
      memset(mDynamicBuffer,0,BUF_SIZE*sizeof(short));
      mSound = 0;
      mChannel = -1;
      mDynamicChunk.allocated = 0;
      mDynamicChunk.abuf = (Uint8 *)mDynamicBuffer;
      mDynamicChunk.alen = BUF_SIZE * sizeof(short) * STEREO_SAMPLES; // bytes
      mDynamicChunk.volume = MIX_MAX_VOLUME;
      mDynamicFillPos = 0;
      mSoundPos0 = 0;
      mDynamicDataDue = 0;
      loopsPending = 0;

      mBufferAheadSamples = 0;//mFrequency / 20; // 50ms buffer

      // Allocate myself a channel
      for(int i=0;i<sMaxChannels;i++)
         if (!sUsedChannel[i])
         {
            IncRef();
            sDoneChannel[i] = false;
            sUsedChannel[i] = true;
            mChannel = i;
            break;
         }

      if (mChannel>=0)
      {
         FillBuffer(inBytes,true);
         // Just once ...
         if (mDynamicFillPos<1024)
         {
            mDynamicRequestPending = true;
            mDynamicChunk.alen = mDynamicFillPos * sizeof(short) * STEREO_SAMPLES;
            if (Mix_PlayChannel( mChannel , &mDynamicChunk,  0 ))
              onChannelDone(mChannel);
         }
         else
         {
            mDynamicRequestPending = false;
            // TODO: Lock?
            if (Mix_PlayChannel( mChannel , &mDynamicChunk,  -1 )<0)
               onChannelDone(mChannel);
         }
         if (!sDoneChannel[mChannel])
         {
            mSoundPos0 = getMixerTime(0);

            Mix_Volume( mChannel, inTransform.volume*MIX_MAX_VOLUME );
         }
      }
   }

   void FillBuffer(const ByteArray &inBytes,bool inFirst)
   {
      int time_samples = inBytes.Size()/sizeof(float)/STEREO_SAMPLES;
      const float *buffer = (const float *)inBytes.Bytes();
      enum { MASK = BUF_SIZE - 1 };

      for(int i=0;i<time_samples;i++)
      {
         int mono_pos =  (i+mDynamicFillPos) & MASK;
         mDynamicBuffer[ mono_pos<<1 ] = *buffer++ * ((1<<15)-1);
         mDynamicBuffer[ (mono_pos<<1) + 1 ] = *buffer++ * ((1<<15)-1);
      }

      int soundTime = getMixerTime(mSoundPos0);

      if (mDynamicFillPos<soundTime && !inFirst)
         ELOG("Too slow - FillBuffer %d / %d)", mDynamicFillPos, soundTime );
      mDynamicFillPos += time_samples;
      if (time_samples<1024 && !mDynamicRequestPending)
      {
         mDynamicRequestPending = true;
         for(int i=0;i<2048;i++)
         {
            int mono_pos =  (i+mDynamicFillPos) & MASK;
            mDynamicBuffer[ mono_pos<<1 ] = 0;
            mDynamicBuffer[ (mono_pos<<1) + 1 ] = 0;
         }

         #ifndef EMSCRIPTEN
         int samples_left = (int)mDynamicFillPos - (int)(soundTime);
         int ticks_left = samples_left*1000/44100;
         //printf("Expire in %d (%d)\n", samples_left, ticks_left );
         Mix_ExpireChannel(mChannel, ticks_left>0 ? ticks_left : 1 );
         #endif
      }
   }
 
   ~SDLSoundChannel()
   {
      delete [] mDynamicBuffer;

      if (mSound)
         mSound->DecRef();
   }

   void CheckDone()
   {
      if (mChannel>=0 && sDoneChannel[mChannel])
      {
         if (loopsPending!=0 && mChunk)
         {
            mOffsetChunk.alen = mChunk->alen;
            mOffsetChunk.abuf = mChunk->abuf;
            if (Mix_PlayChannel( mChannel , &mOffsetChunk, loopsPending<0 ? -1 : loopsPending-1 )==0)
            {
               startSample = endSample = sSoundPos;
               loopsPending = 0;
               sDoneChannel[mChannel] = false;
               return;
            }
         }

         sDoneChannel[mChannel] = false;
         int c = mChannel;
         mChannel = -1;
         DecRef();
         sUsedChannel[c] = 0;
         endSample = sSoundPos;
      }
   }

   bool isComplete()
   {
      CheckDone();
      return mChannel < 0;
   }
   double getLeft() { return 1; }
   double getRight() { return 1; }
   double setPosition(const float &inFloat) { return 1; }
   void stop() 
   {
      if (mChannel>=0)
         Mix_HaltChannel(mChannel);

      //CheckDone();
   }

   double getPosition()
   {
      if (!sMusicFrequency || !mChunk || !mChunk->alen)
         return 0.0;

      int samples = mChunk->alen / (sizeof(short)*STEREO_SAMPLES);
      return (playing ? (sSoundPos - startSample) % samples : endSample - startSample)*1000.0/sMusicFrequency;
   }


   double getDataPosition()
   {
      return getMixerTime(mSoundPos0)*1000.0/mFrequency;
   }
   bool needsData()
   {
      if (!mDynamicBuffer || mDynamicRequestPending)
         return false;

      int soundTime = getMixerTime( mSoundPos0 );
      if (mDynamicDataDue<=soundTime + mBufferAheadSamples)
      {
         mDynamicRequestPending = true;
         return true;
      }

      return false;

   }

   void addData(const ByteArray &inBytes)
   {
      mDynamicRequestPending = false;
      int soundTime = getMixerTime(mSoundPos0);
      mDynamicDataDue = mDynamicFillPos;
      FillBuffer(inBytes,false);
   }


   Object    *mSound;
   Mix_Chunk *mChunk;
   int       mChannel;

   int   startSample;
   int   endSample;
   bool  playing;
   int   loopsPending;


   Mix_Chunk mOffsetChunk;
   Mix_Chunk mDynamicChunk;
   short    *mDynamicBuffer;
   unsigned int  mDynamicFillPos;
   unsigned int  mSoundPos0;
   int       mDynamicDataDue;
   bool      mDynamicRequestPending;
   int       mFrequency;
   Uint16    mFormat;
   int       mChannels;
   int       mBufferAheadSamples;
};

SoundChannel *CreateSdlSyncChannel(const ByteArray &inBytes,const SoundTransform &inTransform,
    SoundDataFormat inDataFormat,bool inIsStereo, int inRate) 
{
   if (!Init())
      return 0;
   return new SDLSoundChannel(inBytes,inTransform);
}



class SDLSound : public Sound
{
   std::string mError;
   Mix_Chunk *mChunk;
   std::string filename;
   bool        loaded;
   int         frequency;
   Uint16      format;
   int         channels;
   double      duration;
   INmeSoundData *soundData;

public:
   SDLSound(const std::string &inFilename)
   {
      IncRef();
      filename = inFilename;
      mChunk = 0;
      loaded = false;
      frequency = 0;
      format = 0;
      channels = 0;
      duration = 0.0;
      soundData = 0;

      if (Init())
         loadChunk();
   }

   SDLSound(const unsigned char *inData, int len)
   {
      loaded = false;
      IncRef();
      if (Init())
      {
         mChunk = Mix_LoadWAV_RW(SDL_RWFromConstMem(inData, len), 1);
         onChunk();
      }
   }

   ~SDLSound()
   {
      if (mChunk)
         Mix_FreeChunk( mChunk );

      if (soundData)
         soundData->release();
   }

   const char *getEngine() { return "sdl sound"; }

   void loadChunk()
   {
      #ifdef HX_MACOS
      char name[1024];
      GetBundleFilename(filename.c_str(),name,1024);
      #else
      const char *name = filename.c_str();
      #endif

      mChunk = Mix_LoadWAV(name);
      //printf("Loaded wav : %s\n", name);

      if (!mChunk)
      {
         ByteArray resource(filename.c_str());
         if (resource.Ok())
         {
            int n = resource.Size();
            if (n>0)
            {
               #ifndef NME_SDL12
               mChunk = Mix_LoadWAV_RW(SDL_RWFromConstMem(resource.Bytes(),n),false);
               #else
               mChunk = Mix_LoadWAV_RW(SDL_RWFromConstMem(resource.Bytes(),2));
               #endif
            }
            if (!mChunk)
            {
               soundData = INmeSoundData::create(resource.Bytes(),n,SoundForceDecode);
               if (soundData)
               {
                  Uint8 *data = (Uint8 *)soundData->decodeAll();
                  if (data)
                  {
                     int bytes = soundData->getDecodedByteCount();
                     mChunk = Mix_QuickLoad_RAW(data, bytes);
                  }
               }
               
            }
         }
      }

      onChunk();
   }

   void onChunk()
   {
      loaded = true;
      if (mChunk)
      {
         Mix_QuerySpec(&frequency, &format, &channels);
         int bytes = mChunk->alen;
         if (bytes && frequency && channels)
            duration = (double)bytes/ (frequency*channels*sizeof(short) );
      }
      else
      {
         mError = SDL_GetError();
         // ELOG("Error %s (%s)", mError.c_str(), name );
      }
   }
  
   double getLength()
   {
     return duration*1000.0;
     //#if defined(DYNAMIC_SDL) || defined(WEBOS)
   }

   // Will return with one ref...
   SoundChannel *openChannel(double startTime, int loops, const SoundTransform &inTransform)
   {
      if (!loaded)
         loadChunk();
      if (!mChunk)
         return 0;
      return new SDLSoundChannel(this,mChunk,startTime, loops,inTransform);
   }
   int getBytesLoaded() { return mChunk ? mChunk->alen : 0; }
   int getBytesTotal() { return mChunk ? mChunk->alen : 0; }
   bool ok() { return mChunk; }
   std::string getError() { return mError; }
};

// ---  Using "Mix_Music" API ----------------------------------------------------


class SDLMusicChannel : public SoundChannel
{
public:
   SDLMusicChannel(Object *inSound, Mix_Music *inMusic, double inStartTime, int inLoops,
                  const SoundTransform &inTransform)
   {
      mMusic = inMusic;
      mSound = inSound;
      mSound->IncRef();

      mPlaying = false;
      if (mMusic)
      {
         mPlaying = true;
         sUsedMusic = this;
         sDoneMusic = false;
         mStartTime = SDL_GetTicks();
         mLength = 0;
         IncRef();

         if (Mix_PlayMusic( mMusic, inLoops<0 ? -1 : inLoops==0 ? 0 : inLoops-1 )<0)
         {
            onMusicDone();
         }
         else
         {
            Mix_VolumeMusic( inTransform.volume*MIX_MAX_VOLUME );
            if (inStartTime > 0)
            {
               // Should be 'almost' at start
               //Mix_RewindMusic();
               #ifndef EMSCRIPTEN
               Mix_SetMusicPosition(inStartTime*0.001); 
               #else
               inStartTime = 0;
               #endif
               mStartTime = SDL_GetTicks() - inStartTime;
            }
            // Mix_SetPanning not available for music
         }
      }
   }
   ~SDLMusicChannel()
   {
      mSound->DecRef();
   }

   void CheckDone()
   {
      if (mPlaying && (sDoneMusic || (sUsedMusic!=this)) )
      {
         mLength = SDL_GetTicks () - mStartTime;
         mPlaying = false;
         if (sUsedMusic == this)
         {
            sUsedMusic = 0;
            sDoneMusic = false;
         }
         DecRef();
      }
   }

   bool isComplete()
   {
      CheckDone();
      return !mPlaying;
   }
   double getLeft() { return 1; }
   double getRight() { return 1; }
   double getPosition() { return mPlaying ? SDL_GetTicks() - mStartTime : mLength; }
   double setPosition(const float &inFloat) { return 1; }

   void stop() 
   {
      if (mMusic)
         Mix_HaltMusic();
   }
   void setTransform(const SoundTransform &inTransform) 
   {
      if (mMusic>=0)
         Mix_VolumeMusic( inTransform.volume*MIX_MAX_VOLUME );
   }

   bool      mPlaying;
   Object    *mSound;
   int       mStartTime;
   int       mLength;
   Mix_Music *mMusic;
};


class SDLMusic : public Sound
{
   bool loaded;
   std::string filename;
   std::vector<unsigned char> reso;
   double duration;

public:
   SDLMusic(const std::string &inFilename)
   {
      filename = inFilename;
      loaded = false;
      mMusic = 0;
      duration = 0;

      IncRef();
      if (gSDLAudioState!=sdaNotInit)
      {
         if (Init())
            loadMusic();
         else
            DecRef();
      }
   }

  const char *getEngine() { return "sdl music"; }


   void loadMusic()
   {
      loaded = true;
      #ifdef HX_MACOS
      char name[1024];
      GetBundleFilename(filename.c_str(),name,1024);
      #else
      const char *name = filename.c_str();
      #endif

      mMusic = Mix_LoadMUS(name);
      if (mMusic)
      {
         INmeSoundData *stream = INmeSoundData::create(name,SoundJustInfo);
         if (stream)
         {
            duration = stream->getDuration() * 1000.0;
            stream->release();
         }
      }
      else
      {
         ByteArray resource(filename.c_str());
         if (resource.Ok())
         {
            int n = resource.Size();
            if (n>0)
            {
               reso.resize(n);
               memcpy(&reso[0], resource.Bytes(), n);
               #ifdef NME_SDL2
               mMusic = Mix_LoadMUS_RW(SDL_RWFromConstMem(&reso[0], reso.size()),false);
               #else
               mMusic = Mix_LoadMUS_RW(SDL_RWFromConstMem(&reso[0], reso.size()));
               #endif

               if (mMusic)
               {
                  INmeSoundData *stream = INmeSoundData::create(&reso[0], reso.size(),SoundJustInfo);
                  if (stream)
                  {
                     duration = stream->getDuration() * 1000.0;
                     stream->release();
                  }
                  else
                  {
                     ELOG("Could not determine music length - assume 60");
                     duration = 60000.0;
                  }
               }
            }
         }
      }


      if (!mMusic)
      {
         mError = SDL_GetError();
         ELOG("Error in music %s (%s)", mError.c_str(), name );
      }
   }
   
   SDLMusic(const unsigned char *inData, int len)
   {
      IncRef();
      loaded = true;
      
      reso.resize(len);
      memcpy(&reso[0], inData, len);

      #ifdef NME_SDL2
      mMusic = Mix_LoadMUS_RW(SDL_RWFromConstMem(&reso[0], len),false);
      #else
      mMusic = Mix_LoadMUS_RW(SDL_RWFromConstMem(&reso[0], len));
      #endif
      if ( mMusic == NULL )
      {
         mError = SDL_GetError();
         ELOG("Error in music with len (%d)", len );
      }
   }
   ~SDLMusic()
   {
      if (mMusic)
      {
         Mix_FreeMusic( mMusic );
      }
   }
   double getLength()
   {
      return duration;
   }
   // Will return with one ref...
   SoundChannel *openChannel(double startTime, int loops, const SoundTransform &inTransform)
   {
      if (!loaded)
         loadMusic();

      if (!mMusic)
         return 0;
      return new SDLMusicChannel(this,mMusic,startTime, loops,inTransform);
   }
   int getBytesLoaded() { return mMusic ? 100 : 0; }
   int getBytesTotal() { return mMusic ? 100 : 0; }
   bool ok() { return mMusic; }
   std::string getError() { return mError; }


   std::string mError;
   Mix_Music *mMusic;
};

// --- External Interface -----------------------------------------------------------


Sound *CreateSdlSound(const std::string &inFilename,bool inForceMusic)
{
   Sound *sound = inForceMusic ? 0 :  new SDLSound(inFilename);

   if (!sound || !sound->ok())
   {
      if (sound) sound->DecRef();
      sound = new SDLMusic(inFilename);
   }
   return sound;
}

Sound *CreateSdlSound(const unsigned char *inData, int len, bool inForceMusic)
{
   Sound *sound = inForceMusic ? 0 : new SDLSound(inData, len);
   if (!sound || !sound->ok())
   {
      if (sound) sound->DecRef();
      sound = new SDLMusic(inData, len);
   }
   return sound;
}


}