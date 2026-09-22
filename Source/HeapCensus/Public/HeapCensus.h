// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * The HeapCensus module.
 *
 * Its loading phase is PreDefault, and the reason is the garbage-collection hooks: the first collections of a
 * session happen while the first map is coming up, they are among the most expensive of the run, and a plugin
 * that starts listening at Default has already missed them. PreDefault is late enough that the UObject system
 * is up and early enough that nothing interesting has been collected yet.
 *
 * The project settings are still read later, on OnPostEngineInit, because a UDeveloperSettings CDO read
 * during module start-up is a CDO read before the config system has finished with it. The census runs on its
 * built-in defaults until then, and nothing is sampled at all before the settling window is over.
 */
class FHeapCensusModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface interface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface interface

private:
	/** Kept so the callback can be taken off a Core delegate that outlives this module's DLL. */
	FDelegateHandle PostEngineInitHandle;
};
