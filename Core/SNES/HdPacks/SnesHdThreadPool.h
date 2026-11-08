#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Reserve de threads permanente pour les passes du filtre HD (rendu Mode 7, composition,
// reflets, eclairage, atmosphere). Les threads sont crees une seule fois, au demarrage du
// filtre, au lieu d'etre recrees a chaque image. Run() bloque jusqu'a la fin de toutes les
// taches ; le thread appelant participe au travail. Un seul appelant a la fois (le filtre).
class SnesHdThreadPool
{
private:
	std::vector<std::thread> _workers;
	std::mutex _m;
	std::condition_variable _cvWork;
	std::condition_variable _cvDone;
	const std::function<void(int)>* _job = nullptr;
	int _count = 0;   // nombre de taches du job courant
	int _next = 0;    // prochaine tache a prendre
	int _done = 0;    // taches terminees
	int _active = 0;  // workers occupes sur le job courant
	uint64_t _gen = 0;
	bool _stop = false;

	void WorkerLoop()
	{
		uint64_t seen = 0;
		std::unique_lock<std::mutex> lk(_m);
		while(true) {
			_cvWork.wait(lk, [&]() { return _stop || _gen != seen; });
			if(_stop) return;
			seen = _gen;
			_active++;
			while(_job && _next < _count) {
				int i = _next++;
				const std::function<void(int)>* job = _job;
				lk.unlock();
				(*job)(i);
				lk.lock();
				_done++;
			}
			_active--;
			if(_done >= _count && _active == 0) _cvDone.notify_all();
		}
	}

public:
	SnesHdThreadPool() = default;
	~SnesHdThreadPool() { Stop(); }

	// total = nombre de threads utilises, thread appelant compris. total <= 1 : pas de worker.
	void Start(int total)
	{
		Stop();
		for(int i = 1; i < total; i++) _workers.emplace_back([this]() { WorkerLoop(); });
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> lk(_m);
			_stop = true;
		}
		_cvWork.notify_all();
		for(auto& w : _workers) if(w.joinable()) w.join();
		_workers.clear();
		_stop = false;
	}

	// Nombre de threads disponibles pour un Run (workers + appelant).
	int Size() const { return (int)_workers.size() + 1; }

	// Execute fn(0) ... fn(count - 1) en parallele et attend la fin de toutes les taches.
	void Run(int count, const std::function<void(int)>& fn)
	{
		if(count <= 0) return;
		if(_workers.empty() || count == 1) {
			for(int i = 0; i < count; i++) fn(i);
			return;
		}
		std::unique_lock<std::mutex> lk(_m);
		_job = &fn;
		_count = count;
		_next = 0;
		_done = 0;
		_gen++;
		lk.unlock();
		_cvWork.notify_all();
		lk.lock();
		while(_next < _count) {
			int i = _next++;
			lk.unlock();
			fn(i);
			lk.lock();
			_done++;
		}
		_cvDone.wait(lk, [&]() { return _done >= _count && _active == 0; });
		_job = nullptr;
	}
};
