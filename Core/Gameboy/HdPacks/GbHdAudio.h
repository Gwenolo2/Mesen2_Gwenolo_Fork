#pragma once
#include "pch.h"
#include <memory>
#include "Gameboy/HdPacks/GbHdPackLoader.h"
#include "NES/HdPacks/OggMixer.h"
#include "Shared/Emulator.h"
#include "Shared/Audio/SoundMixer.h"
#include "Shared/MessageManager.h"

// Musique OGG des packs HD Game Boy (<bgm> + <play_conditional_bgm>), comme le NES (HdAudioDevice).
// Le filtre HD evalue les conditions a chaque image et publie la piste voulue dans GbHdPackData::BgmRequest ;
// Update() est appele une fois par image sur le thread de l'emulation (Gameboy::RunFrame), le meme que le
// mixage (SoundMixer::PlayAudioBuffer) : l'OggMixer, sans verrou, n'est donc jamais touche par deux threads.
class GbHdAudio
{
private:
	Emulator* _emu = nullptr;
	const GbHdPackData* _pack = nullptr;
	std::unique_ptr<OggMixer> _mixer;
	int32_t _current = -1; // demande en cours de lecture : (piste << 1) | boucle, -1 = rien

public:
	GbHdAudio(Emulator* emu, const GbHdPackData* pack) : _emu(emu), _pack(pack)
	{
		_mixer.reset(new OggMixer());
		_mixer->SetBgmVolume(128);
		_mixer->SetSfxVolume(128);
		_emu->GetSoundMixer()->RegisterAudioProvider(_mixer.get());
	}

	~GbHdAudio()
	{
		_emu->GetSoundMixer()->UnregisterAudioProvider(_mixer.get());
	}

	void Update()
	{
		const int32_t req = _pack->BgmRequest ? _pack->BgmRequest->load(std::memory_order_relaxed) : -1;
		if(req == _current) {
			return; // meme piste (ou rien) : on laisse jouer, sans redemarrer
		}
		_current = req;
		if(req < 0) {
			_mixer->StopBgm();
			return;
		}
		const int trackId = req >> 1;
		const bool loop = (req & 1) != 0;
		auto it = _pack->BgmFiles.find(trackId);
		if(it == _pack->BgmFiles.end()) {
			MessageManager::Log("[HD-GB-AUDIO] piste " + std::to_string(trackId >> 8) + "," + std::to_string(trackId & 0xFF) + " sans <bgm> correspondant");
			_mixer->StopBgm();
			return;
		}
		_mixer->SetPlaybackOptions(loop ? 1 : 0);
		_mixer->Play(it->second.Filename, false, 0, it->second.LoopPosition);
		_mixer->SetPlaybackOptions(loop ? 1 : 0);
	}
};
