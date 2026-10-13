#include "pch.h"
#include "NES/HdPacks/HdAudioDevice.h"
#include "NES/HdPacks/HdData.h"
#include "NES/HdPacks/OggMixer.h"
#include "NES/NesConsole.h"
#include "Shared/MessageManager.h"
#include "Shared/Emulator.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/Serializer.h"

//added by Gwenolo:(for log files generated)
#include <fstream>
#include <iostream>

HdAudioDevice::HdAudioDevice(Emulator* emu, HdPackData* hdData)
{
	_emu = emu;
	_hdData = hdData;
	_album = 0;
	_playbackOptions = 0;
	_trackError = false;
	_sfxVolume = 128;
	_bgmVolume = 128;

	_oggMixer.reset(new OggMixer());
	_oggMixer->SetBgmVolume(_bgmVolume);
	_oggMixer->SetSfxVolume(_sfxVolume);
	_emu->GetSoundMixer()->RegisterAudioProvider(_oggMixer.get());
}

HdAudioDevice::~HdAudioDevice()
{
	_emu->GetSoundMixer()->UnregisterAudioProvider(_oggMixer.get());
}

void HdAudioDevice::Serialize(Serializer& s)
{
	int32_t trackOffset = 0;
	if(s.IsSaving()) {
		trackOffset = _oggMixer->GetBgmOffset();
		if(trackOffset < 0) {
			_lastBgmTrack = -1;
		}
		SV(_album); SV(_lastBgmTrack); SV(trackOffset); SV(_sfxVolume); SV(_bgmVolume); SV(_playbackOptions);

		//added by Gwenolo: {
		SV(_currentAdditionalBgmTrack);		SV(_currentStdBgmTrack); SV(_currentAdditionalSfx); SV(_currentAdditionalBgmTrack_LoopFlag);
		// }
	} else {
		SV(_album); SV(_lastBgmTrack); SV(trackOffset); SV(_sfxVolume); SV(_bgmVolume); SV(_playbackOptions);
		
		//added by Gwenolo: {
		SV(_currentAdditionalBgmTrack);		SV(_currentStdBgmTrack); SV(_currentAdditionalSfx); SV(_currentAdditionalBgmTrack_LoopFlag);
		

		if(_currentAdditionalBgmTrack != -1 && trackOffset > 0) {
			PlayBgmTrack(_currentAdditionalBgmTrack, trackOffset);
			_currentStdBgmTrack = -1;
		}
		else // }
			if(_lastBgmTrack != -1 && trackOffset > 0) {
			PlayBgmTrack(_lastBgmTrack, trackOffset);
		}
		_oggMixer->SetBgmVolume(_bgmVolume);
		_oggMixer->SetSfxVolume(_sfxVolume);
		_oggMixer->SetPlaybackOptions(_playbackOptions);
	}
}

bool HdAudioDevice::PlayBgmTrack(int trackId, uint32_t startOffset)
{
	auto result = _hdData->BgmFilesById.find(trackId);
	if(result != _hdData->BgmFilesById.end()) {
		if(_oggMixer->Play(result->second.Filename, false, startOffset, result->second.LoopPosition)) {
			_lastBgmTrack = trackId;
			return true;
		}
	} else {
		MessageManager::Log("[HDPack] Invalid album+track combination: " + std::to_string(_album) + ":" + std::to_string(trackId & 0xFF));
	}
	return false;
}


bool HdAudioDevice::PlaySfx(uint8_t sfxNumber)
{
	auto result = _hdData->SfxFilesById.find(_album * 256 + sfxNumber);
	if(result != _hdData->SfxFilesById.end()) {
		return !_oggMixer->Play(result->second, true, 0, 0);
	} else {
		MessageManager::Log("[HDPack] Invalid album+sfx number combination: " + std::to_string(_album) + ":" + std::to_string(sfxNumber));
		return false;
	}
}

void HdAudioDevice::ProcessControlFlags(uint8_t flags)
{
	_oggMixer->SetPausedFlag((flags & 0x01) == 0x01);
	if(flags & 0x02) {
		_oggMixer->StopBgm();
	}
	if(flags & 0x04) {
		_oggMixer->StopSfx();
	}
}

void HdAudioDevice::GetMemoryRanges(MemoryRanges & ranges)
{
	bool useAlternateRegisters = (_hdData->OptionFlags & (int)HdPackOptions::AlternateRegisterRange) == (int)HdPackOptions::AlternateRegisterRange;
	ranges.SetAllowOverride();

	if(useAlternateRegisters) {
		for(int i = 0; i < 7; i++) {
			ranges.AddHandler(MemoryOperation::Write, 0x3002 + i * 0x10);
		}
		ranges.AddHandler(MemoryOperation::Read, 0x4018);
		ranges.AddHandler(MemoryOperation::Read, 0x4019);
	} else {
		ranges.AddHandler(MemoryOperation::Any, 0x4100, 0x4106);
	}
}

// ADDED by Gwenolo
/*-----------------------------------------------------------*/

/*bool HdAudioDevice::PlayAdditionalSfx(uint8_t sfxNumber)
{
	auto result = _hdData->SfxFilesById.find(_album * 256 + sfxNumber);
	if(result != _hdData->SfxFilesById.end()) {
		return !_oggMixer->Play(result->second, true, 0, 0);
	} else {
		MessageManager::Log("[HDPack] Invalid album+sfx number combination: " + std::to_string(_album) + ":" + std::to_string(sfxNumber));
		return false;
	}
	
	return true;
}
*/


/*--------------------------------------------------------------------------------------*/
void HdAudioDevice::LogAdditionalBgmTracks_and_Conds(string logfilepath, HdPackData* _hdData)

{
	std::ofstream logfile(logfilepath, std::ios_base::app); 
	logfile << "Size of PlayAdditionalBgmActions List  : " <<  _hdData->PlayAdditionalBgmActions.size() << std::endl << std::endl;

	for(auto& action : _hdData->PlayAdditionalBgmActions) {
		string filename = _hdData->BgmFilesById[action.TrackId].Filename;
		int loopPosition = _hdData->BgmFilesById[action.TrackId].LoopPosition;

		logfile << "          -- trackID : " << action.TrackId << " - filename: "<< filename<<" - loop : " << action.loop  << " on sample pos : "<< loopPosition  <<  std::endl << "          >   conditions : " << std::endl;

		bool allCondMet = true;

		for(auto& condition : action.Conditions) {
			bool met = condition->CheckCondition(0, 0, nullptr);
			if(!met) { allCondMet = false; }

			logfile << "                       >>>   condition : " << " - " << condition->ToString() << "-  MET?  -->  " << met << std::endl;
			
		}
		logfile << "                          -- ALL CONDITIONS >> : " << " ALL MET ??? :" << allCondMet << std::endl;
		
	}
	logfile << "  TrackId_For_First_AdditionalBgm_Conds_Met : " << Init_data_For_First_AdditionalBgm_Conds_Met(_hdData) << std::endl;
	logfile.flush();
	logfile << std::endl;
	logfile.close();
	
}
/*--------------------------------------------------------------------------------------*/
void HdAudioDevice::LogAdditionalSfx_and_Conds(string logfilepath, HdPackData* _hdData)

{
	std::ofstream logfile(logfilepath, std::ios_base::app);
	logfile << "Size of PlayAdditionalSfxActions List  : " << _hdData->PlayAdditionalSfxActions.size() << std::endl << std::endl;

	for(auto& action : _hdData->PlayAdditionalSfxActions) {
		logfile << "          -- SfxID : " << action.SfxId << std::endl << "          >   conditions : " << std::endl;

		bool allCondMet = true;

		for(auto& condition : action.Conditions) {
			bool met = condition->CheckCondition(0, 0, nullptr);
			if(!met) { allCondMet = false; }

			logfile << "                       >>>   condition : " << " - " << condition->ToString() << "-  MET?  -->  " << met << std::endl;

		}
		logfile << "                          -- ALL CONDITIONS >> : " << " ALL MET ??? :" << allCondMet << std::endl;

	}
	logfile << "  TrackId_For_First_AdditionalBgm_Conds_Met : " << SfxId_For_First_AdditionalSfx_Conds_Met(_hdData) << std::endl;
	logfile.flush();
	logfile << std::endl;
	logfile.close();

}
/*----------------------------------------------------------------------------------------------------------------*/
void HdAudioDevice::Log_TrackId_For_First_AdditionalBgm_Conds_Met(string logfilepath, HdPackData* _hdData)

{
	std::ofstream logfile(logfilepath, std::ios_base::app);

	logfile << "[FRAME:" << _emu->GetFrameCount() << "]" << "TrackId_For_First_AdditionalBgm_Conds_Met : " << Init_data_For_First_AdditionalBgm_Conds_Met(_hdData) << " - _currentAdditionalBgmTrack :" << _currentAdditionalBgmTrack <<" - _lastBgmTrack:"<< _lastBgmTrack << std::endl;
	logfile.flush();
	logfile.close();

}
/*----------------------------------------------------------------------------------------------------------------*/
void HdAudioDevice::Log_SfxId_For_First_AdditionalSfx_Conds_Met(string logfilepath, HdPackData* _hdData)

{
	std::ofstream logfile(logfilepath, std::ios_base::app);

	logfile << "[FRAME:" << _emu->GetFrameCount() << "]" << "SfxId_For_First_AdditionalSfx_Conds_Met : " << SfxId_For_First_AdditionalSfx_Conds_Met(_hdData) << " - _currentAdditionalSfx :" << _currentAdditionalSfx << std::endl;
	logfile.flush();
	logfile.close();

}

/*---------------------------------------------------------*/
int HdAudioDevice::Init_data_For_First_AdditionalBgm_Conds_Met(HdPackData* _hdData)
{
	_currentAdditionalBgmTrack_LoopFlag = false;
	for(auto& action : _hdData->PlayAdditionalBgmActions) {
		
		bool allCondMet = true;

		for(auto& condition : action.Conditions) {
			bool met = condition->CheckCondition(0, 0, nullptr);
			if(!met) { allCondMet = false; }

		}
		if (allCondMet)  { 
			_currentAdditionalBgmTrack_LoopFlag = action.loop;
			return action.TrackId; 
			break;
		}
	}

	return -1;
}
/*--------------------------------------------------------------------------------*/
bool HdAudioDevice::Is_Looping_First_AdditionalBgm_Conds_Met(HdPackData* _hdData)
{
	//std::ofstream logfile("D:\\Documents\\Mesen2\\HdPacks\\Zelda2__test_audio\\Is_Looping_First_AdditionalBgm_Conds_Met.log", std::ios_base::app);

	for(auto& action : _hdData->PlayAdditionalBgmActions) {
		bool allCondMet = true;
		//logfile << "[FRAME:" << _emu->GetFrameCount() << "]" <<  " Action.Track Id :" << action.TrackId << " Loop? = " << action.loop << std::endl;


		for(auto& condition : action.Conditions) {
			//logfile << "              "  << condition->GetConditionName()  <<  "  " << condition->CheckCondition(0, 0, nullptr) << std::endl; logfile.flush();

			bool met = condition->CheckCondition(0, 0, nullptr);
			if(!met) { 
				allCondMet = false; 
				//logfile << "                     " << " Action.Track Id :" << condition->GetConditionName() << " false? = " << allCondMet << " --> Return false;break;  " << std::endl; logfile.flush();
				break;
			}

		}
		if(    allCondMet==true && action.loop==true) {
			//logfile << " RETURNING Action.Track Id :" << action.TrackId << " Loop? = " << "action.loop TRUE" << std::endl << std::endl; logfile.flush(); logfile.close();
			return true ;
			break;
		}
		else if(allCondMet == false || action.loop != true) {
			//logfile << " RETURNING Action.Track Id :" << action.TrackId << " Loop? = " << "action.loop FALSE" << std::endl << std::endl; logfile.flush(); logfile.close();
			return false;
			break;
		}
	}
	
	return false;
}
/*---------------------------------------------------------*/
int HdAudioDevice::SfxId_For_First_AdditionalSfx_Conds_Met(HdPackData* _hdData)
{
	for(auto& action : _hdData->PlayAdditionalSfxActions) {

		bool allCondMet = true;

		for(auto& condition : action.Conditions) {
			bool met = condition->CheckCondition(0, 0, nullptr);
			if(!met) { allCondMet = false; }					
		}
		if(allCondMet) {

			auto resultSfx = _hdData->SfxFilesById.find(action.SfxId);

			string logfilepath = "D:\\Documents\\Mesen2\\HdPacks\\LegendofZelda_MESEN_Patch\\SfxId_For_First_AdditionalSfx_Conds_Met.log";
			std::ofstream logfile(logfilepath, std::ios_base::app);

			logfile << "[FRAME:" << _emu->GetFrameCount() << "]" << "ALL COND MET : SfxId_For_First_AdditionalSfx_Conds_Met : " << action.SfxId << " - filename :" << resultSfx->second << std::endl;
			logfile.flush();
			logfile.close();
			
			return action.SfxId;
			break;
		}
	}
	return -1;
}


void HdAudioDevice::LogSfxFilesById(string logfilepath, HdPackData* _hdData)
{
	std::ofstream logfile(logfilepath, std::ios_base::app);

	for(const auto& pair : _hdData->SfxFilesById) {
		int id = pair.first;
		const std::string& path = pair.second;

		std::cout << "    SFX ID: " << id << " => " << path << std::endl;
	}
	logfile.flush();
	logfile.close();
}

int HdAudioDevice::PlayAdditionalSfx_IfNeeded()
{
	if(!_oggMixer->IsSfxPlaying()) {

		Log_SfxId_For_First_AdditionalSfx_Conds_Met("D:\\Documents\\Mesen2\\HdPacks\\LegendofZelda_MESEN_Patch\\PlayAdditionalSfx_IfNeeded.log", _hdData);
	

		int _trackError = -1;
		int additionalSfxId_to_Play = SfxId_For_First_AdditionalSfx_Conds_Met(_hdData);

		if(additionalSfxId_to_Play == -1) {
			_oggMixer->StopSfx();
			_currentAdditionalSfx = -1;

		} else {
			if (! _oggMixer->IsSfxPlaying() )
			{
				_trackError = PlaySfx(additionalSfxId_to_Play);
				_currentAdditionalSfx = additionalSfxId_to_Play;
			}
		}
		return _trackError;

	}
	else {
		_currentAdditionalSfx = -1;
		return _trackError;

	}
	
}

/*-----------------------------------------------------*/
int HdAudioDevice::PlayAdditionalBgmTrack_IfNeeded()
{
	//std::ofstream logfile("D:\\Documents\\Mesen2\\HdPacks\\Zelda2__test_audio\\PlayAdditionalBgmTrack_IfNeeded.log", std::ios_base::app);

	bool _trackError = true;
	//bool Is_looping = false;
	//uint8_t oggmixer_options = 0;

	int additionalTrackId_to_Play = Init_data_For_First_AdditionalBgm_Conds_Met(_hdData);

	if(additionalTrackId_to_Play == -1) {
	//logfile << "                                                     STEP X " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush();
		if(_currentStdBgmTrack == -1) {
			//logfile << "                                                       STEP Xa " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush();
			_oggMixer->StopBgm();
			_currentAdditionalBgmTrack_LoopFlag = false;
		}
	_currentBgmTrack = -1;
	_currentAdditionalBgmTrack = -1;
	//logfile << "                                                       STEP Xb : Music Stopped - " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush();
	//logfile << std::endl; logfile.flush();  logfile.close();

	return -1;

   }


	//if(additionalTrackId_to_Play != -1) { Is_looping = Is_Looping_First_AdditionalBgm_Conds_Met(_hdData); oggmixer_options = 1; }

	//logfile  << "HdAudioDevice::PlayAdditionalBgmTrack_IfNeeded() : STEP 0 " << "additionalTrackId_to_Play :"<< additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " -  _currentAdditionalBgmTrack_LoopFlag" << _currentAdditionalBgmTrack_LoopFlag << " - oggmixer_options : " << _oggMixer->_options << std::endl; logfile.flush();


	if((additionalTrackId_to_Play != _currentBgmTrack) && (additionalTrackId_to_Play != -1)) {
		//logfile << "                                                    STEP 1 " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " -  _currentAdditionalBgmTrack_LoopFlag" << _currentAdditionalBgmTrack_LoopFlag << " - oggmixer_options : "<< _oggMixer->_options <<std::endl; logfile.flush();
		_oggMixer->SetPlaybackOptions((uint8_t) _currentAdditionalBgmTrack_LoopFlag);
		//_oggMixer->SetPlaybackOptions(Is_looping);
		_trackError = PlayBgmTrack(additionalTrackId_to_Play, 0);
		_oggMixer->SetPlaybackOptions((uint8_t)_currentAdditionalBgmTrack_LoopFlag);
		_currentBgmTrack = additionalTrackId_to_Play; // managed in PlayBgmTrack()
		_currentAdditionalBgmTrack = additionalTrackId_to_Play;
		_currentStdBgmTrack = -1;
		//logfile << "                                                     END STEP 1:  Après IF " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " -  _currentAdditionalBgmTrack_LoopFlag" << _currentAdditionalBgmTrack_LoopFlag << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush(); logfile.close();
		return _trackError;
	}
	else if((additionalTrackId_to_Play == _currentBgmTrack) && (additionalTrackId_to_Play != -1)) {
		
		//logfile << "                                                    STEP 2 " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " -  _currentAdditionalBgmTrack_LoopFlag" << _currentAdditionalBgmTrack_LoopFlag << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush();
		_oggMixer->SetPlaybackOptions((uint8_t)_currentAdditionalBgmTrack_LoopFlag);
		//// USEFUL ???????????? --> cause looping option non working when set to false 
		/*if(_oggMixer->GetBgmOffset() <= 0) {
			//bool Is_looping = Is_Looping_First_AdditionalBgm_Conds_Met(_hdData); // LoopPosition
			_oggMixer->SetPlaybackOptions(Is_looping);

			_trackError = PlayBgmTrack(additionalTrackId_to_Play, 0);
			_oggMixer->SetPlaybackOptions(Is_looping);
			logfile << "                                                         STEP 2a " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << std::endl; logfile.flush();
		}*/
		_currentBgmTrack = additionalTrackId_to_Play; // managed in PlayBgmTrack()
		_currentAdditionalBgmTrack = additionalTrackId_to_Play;
		_currentStdBgmTrack = -1;
		//logfile << "                                                     END STEP 2:  Après IF " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush();

	}
	/*else if(additionalTrackId_to_Play == -1) {
		logfile << "                                                     STEP 3 " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush();
		if(_currentStdBgmTrack == -1) {
			logfile << "                                                       STEP 3a " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush();
			_oggMixer->StopBgm();
		}
		_currentBgmTrack = -1;
		_currentAdditionalBgmTrack = -1;
		logfile << "                                                       STEP 3b " << "additionalTrackId_to_Play :" << additionalTrackId_to_Play << " - _currentBgmTrack : " << _currentBgmTrack << " - oggmixer_options : " << _oggMixer->GetOptions() << std::endl; logfile.flush();
	}*/
	
	//logfile << std::endl; logfile.flush();  logfile.close();
	
	return _trackError;
}


/*-----------------------------------------------------------*/
void HdAudioDevice::WriteRam(uint16_t addr, uint8_t value)
{
	int regNumber = addr > 0x4100 ? (addr & 0xF) : ((addr & 0xF0) >> 4);
	
	std::ofstream logfile("D:\\Documents\\Mesen2\\HdPacks\\Zelda2__test_audio\\HdAudioDeviceWriteRamLog.txt", std::ios_base::app);
	logfile << "[Frame " << _emu->GetFrameCount() << "] AudioDevice::WriteRam  : value=" << std::hex << addr << " - " << std::endl;

	uint32_t actualFrame = _emu->GetFrameCount();
	int additionalBgmId_to_Play = Init_data_For_First_AdditionalBgm_Conds_Met(_hdData);

	switch(regNumber) {
		case 0: _playbackOptions = value; _oggMixer->SetPlaybackOptions(_playbackOptions);

			logfile << " case 0 : value=" << std::hex << (int)value << " (current= " << std::hex << (int)_currentBgmTrack << ")" << std::endl;
			logfile.flush();
			logfile.close();
			break;

		case 1: ProcessControlFlags(value); break;
		case 2:
			_bgmVolume = value; _oggMixer->SetBgmVolume(value);
			logfile << "[                AudioDevice::WriteRam  : CASE 2 value=" << (int)value << " - " << std::endl;
			break;
		case 3: _sfxVolume = value; _oggMixer->SetSfxVolume(value); break;
		case 4: _album = value; break;


		case 5:

			//_trackError = PlayBgmTrack(_album * 256 + value, 0); break;


			logfile << " CASE 5  - ";

			logfile << "         _currentStdBgmTrack before setting it in Case 5: " << (int)_currentAdditionalBgmTrack << std::endl;
			logfile.flush();

			_currentStdBgmTrack = _album * 256 + value;

			// si une conditional Bgm doit jouer
			if(additionalBgmId_to_Play >= 0) {

				// si cette conditional Bgm est differente de l'additional bgm track actuelle
				if(additionalBgmId_to_Play != _currentAdditionalBgmTrack) {
					bool Is_looping = Is_Looping_First_AdditionalBgm_Conds_Met(_hdData); // LoopPosition
					_oggMixer->SetPlaybackOptions(Is_looping);
					_trackError = PlayBgmTrack(additionalBgmId_to_Play, 0);
					_oggMixer->SetPlaybackOptions(Is_looping);
					_currentStdBgmTrack = -1;
					_currentBgmTrack = additionalBgmId_to_Play;
					_currentAdditionalBgmTrack = additionalBgmId_to_Play;
				}

				//sinon 
				else if(additionalBgmId_to_Play == _currentAdditionalBgmTrack) {
					
					if(_oggMixer->GetBgmOffset() <= 0) {
						bool Is_looping = Is_Looping_First_AdditionalBgm_Conds_Met(_hdData); // LoopPosition
						_oggMixer->SetPlaybackOptions(Is_looping);

						_trackError = PlayBgmTrack(additionalBgmId_to_Play, 0);
						_oggMixer->SetPlaybackOptions(Is_looping);
					}
					_currentStdBgmTrack = -1;
					_currentBgmTrack = additionalBgmId_to_Play;
					_currentAdditionalBgmTrack = additionalBgmId_to_Play;
				}
			}
			//else : Une std music doit jouer 
			else {
				_trackError = PlayBgmTrack(_album * 256 + value, 0);
				_currentStdBgmTrack = _album * 256 + value;
				_currentBgmTrack = _album * 256 + value;
				_currentAdditionalBgmTrack = -1;

			}
			
			
			
			/*
			// s'il y a une additional BGM à jouer identique à la STDBGM à jouer : 
			if(additionalBgmId_to_Play >= 0 && additionalBgmId_to_Play == _currentStdBgmTrack) {

				// si cette additionalBGM est differente de celle en cours, on la joue : 
				if (additionalBgmId_to_Play != _currentAdditionalBgmTrack) {
					bool Is_looping = Is_Looping_First_AdditionalBgm_Conds_Met(_hdData); // LoopPosition
					_trackError = PlayBgmTrack(additionalBgmId_to_Play, 0);
					_oggMixer->SetPlaybackOptions(Is_looping);
					_currentStdBgmTrack = _album * 256 + value;
					_currentBgmTrack = additionalBgmId_to_Play;
					_currentAdditionalBgmTrack = additionalBgmId_to_Play;

				}
			}
			
			
			// s'il y a une additional BGM à jouer differente de la STDBGM à jouer : 
			else if(additionalBgmId_to_Play >= 0 && additionalBgmId_to_Play != _currentAdditionalBgmTrack)  {
				bool Is_looping = Is_Looping_First_AdditionalBgm_Conds_Met(_hdData); // LoopPosition
				_trackError = PlayBgmTrack(additionalBgmId_to_Play, 0);
				_oggMixer->SetPlaybackOptions(Is_looping);
				_currentStdBgmTrack = -1;
				_currentBgmTrack = additionalBgmId_to_Play;
				_currentAdditionalBgmTrack = additionalBgmId_to_Play;
			}
			//s'il n'y a pas d'additionalBGM à jouer : 
			else if(additionalBgmId_to_Play ==-1  )
			{
				_trackError = PlayBgmTrack(_album * 256 + value, 0);
				_currentStdBgmTrack = _album * 256 + value;
				_currentBgmTrack = _album * 256 + value;
				_currentAdditionalBgmTrack = -1;
			}
			*/
	
			logfile << "    _currentAdditionalBgmTrack end of Case 5 :" << (int)_currentAdditionalBgmTrack << std::endl;
			logfile << "    _currentStdBgmTrack end of Case 5 :" << (int)_currentStdBgmTrack << std::endl;
			logfile << "    _currentBgmTrack end Of Case 5   :" << (int)_currentBgmTrack << std::endl  << std::endl;
			logfile.flush();
			logfile.close();
			break;
					
					
		case 6: //SFX 
				_trackError = PlaySfx(value);	break;

	}
}

/*  // BACKUP DE LA FONCTION D'ORIGINE
void HdAudioDevice::WriteRam(uint16_t addr, uint8_t value)
{
	//$4100/$3002: Playback Options
	//$4101/$3012: Playback Control
	//$4102/$3022: BGM Volume
	//$4103/$3032: SFX Volume
	//$4104/$3042: Album Number
	//$4105/$3052: Play BGM Track
	//$4106/$3062: Play SFX Track
	int regNumber = addr > 0x4100 ? (addr & 0xF) : ((addr & 0xF0) >> 4);

	switch(regNumber) {
		//Playback Options
		//Bit 0: Loop BGM
		//Bit 1-7: Unused, reserved - must be 0
		case 0:
			_playbackOptions = value;
			_oggMixer->SetPlaybackOptions(_playbackOptions);
			break;

		//Playback Control
		//Bit 0: Toggle Pause/Resume (only affects BGM)
		//Bit 1: Stop BGM
		//Bit 2: Stop all SFX
		//Bit 3-7: Unused, reserved - must be 0
		case 1: ProcessControlFlags(value); break;

		//BGM Volume: 0 = mute, 255 = max
		//Also has an immediate effect on currently playing BGM
		case 2:
			_bgmVolume = value;
			_oggMixer->SetBgmVolume(value);
			break;

		//SFX Volume: 0 = mute, 255 = max
		//Also has an immediate effect on all currently playing SFX
		case 3:
			_sfxVolume = value;
			_oggMixer->SetSfxVolume(value);
			break;

		//Album number: 0-255 (Allows for up to 64k BGM and SFX tracks)
		//No immediate effect - only affects subsequent $4FFE/$4FFF writes
		case 4: _album = value; break;

		//Play BGM track (0-255 = track number)
		//Stop the current BGM and starts a new track
		case 5: _trackError = PlayBgmTrack(_album * 256 + value, 0); break;

		//Play sound effect (0-255 = sfx number)
		//Plays a new sound effect (no limit to the number of simultaneous sound effects)
		case 6: _trackError = PlaySfx(value); break;
	}
}
*/

uint8_t HdAudioDevice::ReadRam(uint16_t addr)
{
	//$4100/$4018: Status
	//$4101/$4019: Revision
	//$4102: 'N' (signature to help detection)
	//$4103: 'E'
	//$4103: 'A'

	switch(addr & 0x7) {
		case 0:
			//Status
			return (
				((_musicForcedPlaying || _oggMixer->IsBgmPlaying()) ? 1 : 0) |
				(_oggMixer->IsSfxPlaying() ? 2 : 0) |
				(_trackError ? 4 : 0)
			);

		case 1: return 1; //Revision
		case 2: return 'N'; //NES
		case 3: return 'E'; //Enhanced
		case 4: return 'A'; //Audio
	}

	return 0;
}
