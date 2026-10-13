#pragma once
#include "pch.h"
#include "NES/INesMemoryHandler.h"
#include "Utilities/ISerializable.h"

struct HdPackData;
class Emulator;
class OggMixer;

class HdAudioDevice : public INesMemoryHandler, public ISerializable
{
private:
	Emulator* _emu = nullptr;
	HdPackData* _hdData = nullptr; 
	uint8_t _album = 0;
	uint8_t _playbackOptions = 0;
	bool _trackError = false;
	unique_ptr<OggMixer> _oggMixer;
	int32_t _lastBgmTrack = 0;
	uint8_t _bgmVolume = 0;
	uint8_t _sfxVolume = 0;
	
	bool PlayBgmTrack(int trackId, uint32_t startOffset);

	int _currentBgmTrack = -1;
	bool _musicForcedPlaying = true;

	bool PlaySfx(uint8_t sfxNumber);
	void ProcessControlFlags(uint8_t flags);

protected:
	void Serialize(Serializer& s) override;
	

public:
	HdAudioDevice(Emulator* emu, HdPackData *hdData);
	~HdAudioDevice();

	void GetMemoryRanges(MemoryRanges& ranges) override;
	void WriteRam(uint16_t addr, uint8_t value) override;
	uint8_t ReadRam(uint16_t addr) override;

	/*---------      Added by Gwenolo     -----------*/
	uint32_t _frameMemoryRead = 0;
	int _currentAdditionalBgmTrack = -1;
	bool _currentAdditionalBgmTrack_LoopFlag = false;
	int _currentStdBgmTrack = -1;
	int _currentAdditionalSfx = -1;

	int  PlayAdditionalBgmTrack_IfNeeded();
	int  PlayAdditionalSfx_IfNeeded();
	//bool PlayAdditionalSfx(uint8_t sfxNumber);
	

	bool Is_Looping_First_AdditionalBgm_Conds_Met(HdPackData* _hdData);

	int  Init_data_For_First_AdditionalBgm_Conds_Met(HdPackData* _hdData);
	int  SfxId_For_First_AdditionalSfx_Conds_Met(HdPackData* _hdData);

	void LogAdditionalBgmTracks_and_Conds(string logfilepath, HdPackData* _hdData);
	void Log_TrackId_For_First_AdditionalBgm_Conds_Met(string logfilepath, HdPackData* _hdData);

	void LogSfxFilesById(string logfilepath, HdPackData* _hdData);
	void LogAdditionalSfx_and_Conds(string logfilepath, HdPackData* _hdData);
	void Log_SfxId_For_First_AdditionalSfx_Conds_Met(string logfilepath, HdPackData* _hdData);
	
	/*-----------------------------------------------*/
	

};