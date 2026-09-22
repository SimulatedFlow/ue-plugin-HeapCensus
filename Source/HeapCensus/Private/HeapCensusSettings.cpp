// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "HeapCensusSettings.h"

#include "HeapCensusRecorder.h"

UHeapCensusSettings::UHeapCensusSettings()
{
	// Engine bookkeeping that exists in numbers a project cannot influence, and that swings while packages
	// are loaded and unloaded.
	//
	// The list is three entries long and that is deliberate. Every name on it is something the engine creates
	// on its own account while packages come and go; nothing gameplay-shaped is on it, because a census that
	// quietly hides classes is worse than no census. Whatever this list drops is still inside the object
	// total, and the number of classes it dropped is printed in the counter box, so it can never hide its
	// own effect.
	//
	// Package rises and falls with level streaming, ObjectRedirector with asset renames, and the
	// LinkerPlaceholder family exists only while the editor is patching up a circular Blueprint reference.
	// None of the three is a thing anybody can go and fix.
	IgnoredClassNames = {
		TEXT("Package"),
		TEXT("ObjectRedirector"),
		TEXT("LinkerPlaceholder*"),
	};
}

FName UHeapCensusSettings::GetCategoryName() const
{
	return FName(TEXT("Plugins"));
}

FName UHeapCensusSettings::GetSectionName() const
{
	return FName(TEXT("HeapCensus"));
}

const UHeapCensusSettings& UHeapCensusSettings::Get()
{
	const UHeapCensusSettings* Settings = GetDefault<UHeapCensusSettings>();
	check(Settings);
	return *Settings;
}

#if WITH_EDITOR
void UHeapCensusSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// A threshold typed into the Details panel has to be in force on the next census, not after the next
	// restart. A settings page whose effect you cannot see is a settings page people stop trusting.
	FHeapCensusRecorder::Get().ApplySettings();
}
#endif
