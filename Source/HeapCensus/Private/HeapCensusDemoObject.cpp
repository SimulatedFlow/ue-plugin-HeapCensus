// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "HeapCensusDemoObject.h"

void UHeapCensusDemoObject::SetPayloadKilobytes(int32 Kilobytes)
{
	const int32 Bytes = FMath::Clamp(Kilobytes, 0, 1024) * 1024;

	if (Bytes <= 0)
	{
		Payload.Empty();
		return;
	}

	// Filled rather than merely reserved. An untouched allocation is not necessarily resident memory, and a
	// demonstration that only moves a number in this plugin and nowhere else would be exactly the kind of
	// claim this plugin exists to disprove.
	Payload.SetNumUninitialized(Bytes);
	FMemory::Memset(Payload.GetData(), 0xA5, Bytes);
}
