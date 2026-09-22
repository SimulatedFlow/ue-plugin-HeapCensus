// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "HeapCensusHUD.h"

#include "HeapCensusSubsystem.h"

void AHeapCensusHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!bDrawCounterBox)
	{
		return;
	}

	if (UHeapCensusSubsystem* Census = UHeapCensusSubsystem::Get())
	{
		Census->DrawCounterBox(Canvas);
	}
}
