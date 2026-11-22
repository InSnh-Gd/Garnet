//
//  rdna4_sleep_pure.h — Pure S3 system sleep/wake state machine for Garnet.
//
//  Absorbed and adapted from RX4Darwin/AMDRadeonRDNA4.kext (src/device.cpp
//  sleepWakeMessage and systemSleepWanted, commits 6b6da1e0, 7c8cf3a5, 46f2c250,
//  bb4c5145, 50a0fd1f), Copyright (c) 2026 Sunneva N. Mariu,
//  licensed under the BSD 3-Clause License (see third-party/THIRD-PARTY-LICENSES.txt).
//
#pragma once

#include <stdint.h>

namespace rdna4sleep {

constexpr uint32_t kVramSentinel = 0x5a5a1234u;

enum class Phase {
	Awake = 0,
	Sleeping = 1,
	Waking = 2,
};

enum class Message {
	WillSleep = 0,
	WillNotSleep = 1,
	WillPowerOn = 2,
	HasPoweredOn = 3,
};

struct Actions {
	bool logPci { false };
	bool writeVramMark { false };
	bool checkVramMark { false };
	bool quiesceEngines { false };
	bool resumeEngines { false };
	bool recoverDisplayPads { false };
};

struct State {
	Phase    phase { Phase::Awake };
	bool     coldWakePending { false };
	uint32_t vramMark { 0u };
	uint32_t sleepCount { 0u };
	uint32_t wakeCount { 0u };
	uint32_t abortedSleepCount { 0u };
	uint32_t vramRetainedCount { 0u };
	uint32_t vramLostCount { 0u };

	Actions on_message(Message msg, bool vramWriteOk, uint32_t vramReadBack) {
		Actions act {};
		switch (msg) {
		case Message::WillSleep:
			phase = Phase::Sleeping;
			coldWakePending = false;
			vramMark = vramWriteOk ? kVramSentinel : 0u;
			sleepCount++;
			act.logPci = true;
			act.writeVramMark = true;
			act.quiesceEngines = true;
			break;
		case Message::WillNotSleep:
			phase = Phase::Awake;
			coldWakePending = false;
			vramMark = 0u;
			abortedSleepCount++;
			act.resumeEngines = true;
			break;
		case Message::WillPowerOn:
			phase = Phase::Waking;
			coldWakePending = true;
			act.logPci = true;
			break;
		case Message::HasPoweredOn:
			phase = Phase::Awake;
			coldWakePending = false;
			wakeCount++;
			act.logPci = true;
			act.resumeEngines = true;
			act.recoverDisplayPads = true;
			if (vramMark != 0u) {
				act.checkVramMark = true;
				if (vramReadBack == vramMark) {
					vramRetainedCount++;
				} else {
					vramLostCount++;
				}
				vramMark = 0u;
			}
			break;
		}
		return act;
	}
};

} // namespace rdna4sleep
