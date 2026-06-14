#include "common.h"
#ifdef USE_STEAMAUDIO
#include "sampman.h"
#include "AudioManager.h"

#define STEAMAUDIO_BUILDING_CORE
#include "MusicManager.h"
#include "phonon_interfaces.h"

namespace SteamAudio
{
static IPLContextSettings contextSettings{};
static IPLContext context = nullptr;

static bool8
InitSteamAudio()
{
	contextSettings.version = STEAMAUDIO_VERSION;

	IPLerror errorCode = iplContextCreate(&contextSettings, &context);
	if(errorCode) {
		debug("Initial SteamAudio fatal!\n");
		return false;
	}

	debug("Initial SteamAudio success.\n");
	return true;
}

static void
ShutdownSteamAudio() { debug("Shutdown SteamAudio.\n"); }
}

FILE *fpSampleDataHandle;
int8 gBankLoaded[MAX_SFX_BANKS];
int32 nSampleBankDiscStartOffset[MAX_SFX_BANKS];
int32 nSampleBankSize[MAX_SFX_BANKS];
uintptr nSampleBankMemoryStartAddress[MAX_SFX_BANKS];
int32 _nSampleDataEndOffset;
int32 nPedSlotSfx[MAX_PEDSFX];
int32 nPedSlotSfxAddr[MAX_PEDSFX];
uint8 nCurrentPedSlot;
#ifdef FIX_BUGS
uint32 gPlayerTalkSfx = UINT32_MAX;
void *gPlayerTalkData = 0;
#endif
float *aChannel[NUM_CHANNELS];
uint8 nChannelVolume[NUM_CHANNELS];

cSampleManager SampleManager;
bool8 _bSampmanInitialised = FALSE;

uint32 BankStartOffset[MAX_SFX_BANKS];
uint32 nNumMP3s;

cSampleManager::cSampleManager(void) {}

cSampleManager::~cSampleManager(void) {}

#ifdef EXTERNAL_3D_SOUND
void
cSampleManager::SetSpeakerConfig(int32 nConfig) {}

uint32
cSampleManager::GetMaximumSupportedChannels(void) { return MAXCHANNELS; }

uint32
cSampleManager::GetNum3DProvidersAvailable() { return 1; }

void
cSampleManager::SetNum3DProvidersAvailable(uint32 num) { m_nNumberOfProviders = num; }

char *
cSampleManager::Get3DProviderName(uint8 id)
{
	static char name[64] = "Steam Audio";
	return name;
}

void
cSampleManager::Set3DProviderName(uint8 id, char *name) {}

int8
cSampleManager::GetCurrent3DProviderIndex(void) { return 0; }

int8
cSampleManager::SetCurrent3DProvider(uint8 nProvider) { return nProvider; }
#endif

bool8
cSampleManager::IsMP3RadioChannelAvailable(void) { return nNumMP3s != 0; }


void
cSampleManager::ReleaseDigitalHandle(void) {}

void
cSampleManager::ReacquireDigitalHandle(void) {}

bool8
cSampleManager::Initialise(void)
{
	if(_bSampmanInitialised) { return TRUE; }

	if(!SteamAudio::InitSteamAudio()) { return FALSE; }

	return TRUE;
}

void
cSampleManager::Terminate(void) { SteamAudio::ShutdownSteamAudio(); }

bool8
cSampleManager::CheckForAnAudioFileOnCD(void) { return TRUE; }

char
cSampleManager::GetCDAudioDriveLetter(void) { return '\0'; }

void
cSampleManager::UpdateEffectsVolume(void)
{
	if(_bSampmanInitialised) {
		for(int32 i = 0; i < NUM_CHANNELS; i++) {
			if(GetChannelUsedFlag(i)) {
				if(nChannelVolume[i] != 0) {
					// aChannel[i].SetVolume(m_nEffectsFadeVolume * nChannelVolume[i] * m_nEffectsVolume >> 14);
				}
			}
		}
	}
}

void
cSampleManager::SetEffectsMasterVolume(uint8 nVolume) {}

void
cSampleManager::SetMusicMasterVolume(uint8 nVolume) {}

void
cSampleManager::SetMP3BoostVolume(uint8 nVolume) {}

void
cSampleManager::SetEffectsFadeVolume(uint8 nVolume) {}

void
cSampleManager::SetMusicFadeVolume(uint8 nVolume) {}

void
cSampleManager::SetMonoMode(bool8 nMode) {}

bool8
cSampleManager::LoadSampleBank(uint8 nBank)
{
	ASSERT(nBank < MAX_SFX_BANKS);

	if(CTimer::GetIsCodePaused()) return FALSE;

	if(MusicManager.IsInitialised()
	   && MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE
	   && nBank != SFX_BANK_0) { return FALSE; }

	if(fseek(fpSampleDataHandle, nSampleBankDiscStartOffset[nBank], SEEK_SET) != 0) return FALSE;

	if(fread((void *)nSampleBankMemoryStartAddress[nBank], 1, nSampleBankSize[nBank], fpSampleDataHandle) != nSampleBankSize[nBank]) return FALSE;

	gBankLoaded[nBank] = LOADING_STATUS_LOADED;

	return TRUE;
}

void
cSampleManager::UnloadSampleBank(uint8 nBank)
{
	ASSERT(nBank < MAX_SFX_BANKS);

	gBankLoaded[nBank] = LOADING_STATUS_NOT_LOADED;
}

int8
cSampleManager::IsSampleBankLoaded(uint8 nBank)
{
	ASSERT(nBank < MAX_SFX_BANKS);

	return LOADING_STATUS_NOT_LOADED;
}

uint8
cSampleManager::IsMissionAudioLoaded(uint8 nSlot, uint32 nSample)
{
	ASSERT(nSlot < MISSION_AUDIO_COUNT);

	return LOADING_STATUS_NOT_LOADED;
}

bool8
cSampleManager::LoadMissionAudio(uint8 nSlot, uint32 nSample)
{
	ASSERT(nSlot < MISSION_AUDIO_COUNT);

	return FALSE;
}

uint8
cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	ASSERT(nComment < TOTAL_AUDIO_SAMPLES);

	return LOADING_STATUS_NOT_LOADED;
}


int32
cSampleManager::_GetPedCommentSlot(uint32 nComment) { return -1; }

bool8
cSampleManager::LoadPedComment(uint32 nComment)
{
	ASSERT(nComment < TOTAL_AUDIO_SAMPLES);

	if(CTimer::GetIsCodePaused()) return FALSE;

	// no talking peds during cutsenes or the game end
	if(MusicManager.IsInitialised()) {
		switch(MusicManager.GetMusicMode()) {
		case MUSICMODE_CUTSCENE: {
			return FALSE;

			break;
		}
		}
	}

	if(fseek(fpSampleDataHandle, m_aSamples[nComment].nOffset, SEEK_SET) != 0) return FALSE;

	if(fread((void *)(nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] + PED_BLOCKSIZE * nCurrentPedSlot), 1, m_aSamples[nComment].nSize,
	         fpSampleDataHandle) != m_aSamples[nComment].nSize)
		return FALSE;

	nPedSlotSfx[nCurrentPedSlot] = nComment;

	if(++nCurrentPedSlot >= MAX_PEDSFX) nCurrentPedSlot = 0;

	return TRUE;
}

int32
cSampleManager::GetBankContainingSound(uint32 offset)
{
	if(offset >= BankStartOffset[SFX_BANK_PED_COMMENTS]) return SFX_BANK_PED_COMMENTS;

	if(offset >= BankStartOffset[SFX_BANK_0]) return SFX_BANK_0;

	return INVALID_SFX_BANK;
}

uint32
cSampleManager::GetSampleBaseFrequency(uint32 nSample)
{
	ASSERT(nSample < TOTAL_AUDIO_SAMPLES);
	return 0;
}

uint32
cSampleManager::GetSampleLoopStartOffset(uint32 nSample)
{
	ASSERT(nSample < TOTAL_AUDIO_SAMPLES);
	return 0;
}

int32
cSampleManager::GetSampleLoopEndOffset(uint32 nSample)
{
	ASSERT(nSample < TOTAL_AUDIO_SAMPLES);
	return 0;
}

uint32
cSampleManager::GetSampleLength(uint32 nSample)
{
	ASSERT(nSample < TOTAL_AUDIO_SAMPLES);
	return 0;
}

bool8
cSampleManager::UpdateReverb(void) { return FALSE; }

void
cSampleManager::SetChannelReverbFlag(uint32 nChannel, bool8 nReverbFlag)
{
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}

bool8
cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	ASSERT(nChannel < NUM_CHANNELS);

	uintptr addr;

	if(nSfx < SAMPLEBANK_MAX) {
		if(!IsSampleBankLoaded(nBank)) return FALSE;

		addr = nSampleBankMemoryStartAddress[nBank] + m_aSamples[nSfx].nOffset - m_aSamples[BankStartOffset[nBank]].nOffset;
	}
#ifdef FIX_BUGS
	else if(nSfx >= PLAYER_COMMENTS_START && nSfx <= PLAYER_COMMENTS_END) {
		if(!IsMissionAudioLoaded(MISSION_AUDIO_PLAYER_COMMENT, nSfx)) return FALSE;

		addr = (uintptr)gPlayerTalkData;
	}
#endif
	else {
		int32 i;
		for(i = 0; i < _TODOCONST(3); i++) {
			int32 slot = nCurrentPedSlot - i - 1;
#ifdef FIX_BUGS
			if(slot < 0) slot += ARRAY_SIZE(nPedSlotSfx);
#endif
			if(nSfx == nPedSlotSfx[slot]) {
				addr = (nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] + PED_BLOCKSIZE * slot);
				break;
			}
		}

		if(i == _TODOCONST(3)) return FALSE;
	}

	if(GetChannelUsedFlag(nChannel)) {
		TRACE("Stopping channel %d - really!!!", nChannel);
		StopChannel(nChannel);
	}

	/*
	aChannel[nChannel].Reset();
	if(aChannel[nChannel].HasSource()) {
		aChannel[nChannel].SetSampleData((void *)addr, m_aSamples[nSfx].nSize, m_aSamples[nSfx].nFrequency);
		aChannel[nChannel].SetLoopPoints(0, -1);
		aChannel[nChannel].SetPitch(1.0f);
		return TRUE;
	}*/

	return FALSE;
}

#ifdef EXTERNAL_3D_SOUND
void
cSampleManager::SetChannelEmittingVolume(uint32 nChannel, uint32 nVolume)
{
	ASSERT(nChannel < MAXCHANNELS);

	uint32 vol = nVolume;
	if(vol > MAX_VOLUME) vol = MAX_VOLUME;

	nChannelVolume[nChannel] = vol;

	if(MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE) {
		if(MusicManager.GetCurrentTrack() == STREAMED_SOUND_CUTSCENE_FINALE) nChannelVolume[nChannel] = 0;
		else nChannelVolume[nChannel] >>= 2;
	}

	// no idea, does this one looks like a bug or it's SetChannelVolume ?
	// aChannel[nChannel].SetVolume(m_nEffectsFadeVolume*nChannelVolume[nChannel]*m_nEffectsVolume >> 14);
}

void
cSampleManager::SetChannel3DPosition(uint32 nChannel, float fX, float fY, float fZ)
{
	ASSERT(nChannel < MAXCHANNELS);
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);

	// aChannel[nChannel].SetPosition(-fX, fY, fZ);
}

void
cSampleManager::SetChannel3DDistances(uint32 nChannel, float fMax, float fMin)
{
	ASSERT(nChannel < MAXCHANNELS);
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}
#endif

void
cSampleManager::SetChannelVolume(uint32 nChannel, uint32 nVolume)
{
	ASSERT(nChannel >= MAXCHANNELS);
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}

void
cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	ASSERT(nChannel >= MAXCHANNELS);
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}

void
cSampleManager::SetChannelFrequency(uint32 nChannel, uint32 nFreq)
{
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}

void
cSampleManager::SetChannelLoopPoints(uint32 nChannel, uint32 nLoopStart, int32 nLoopEnd)
{
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}

void
cSampleManager::SetChannelLoopCount(uint32 nChannel, uint32 nLoopCount)
{
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}

bool8
cSampleManager::GetChannelUsedFlag(uint32 nChannel)
{
	ASSERT(nChannel < NUM_CHANNELS);

	//return aChannel[nChannel].IsUsed();
	return FALSE;
}

void
cSampleManager::StartChannel(uint32 nChannel)
{
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	ASSERT(nChannel < MAXCHANNELS+MAX2DCHANNELS);
}

void
cSampleManager::PreloadStreamedFile(uint32 nFile, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);
}

void
cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);
}

void
cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);
}

bool8
cSampleManager::StartStreamedFile(uint32 nFile, uint32 nPos, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

	return FALSE;
}

void
cSampleManager::StopStreamedFile(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

	return 0;
}

void
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, bool8 nEffectFlag, uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);
}

int32
cSampleManager::GetStreamedFileLength(uint8 nStream)
{
	ASSERT(nStream < TOTAL_STREAMED_SOUNDS);

	return 1;
}

bool8
cSampleManager::IsStreamPlaying(uint8 nStream)
{
	ASSERT(nStream < MAX_STREAMS);

	return FALSE;
}

bool8
cSampleManager::InitialiseSampleBanks(void) { return TRUE; }

void
cSampleManager::SetStreamedFileLoopFlag(bool8 nLoopFlag, uint8 nChannel) {}

int8
cSampleManager::AutoDetect3DProviders()
{
	uint32 providerCount = GetNum3DProvidersAvailable();
	if(providerCount > 0) { return 1; }

	return -1;
}

#endif