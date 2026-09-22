// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HeapCensusTypes.h"
#include "Subsystems/EngineSubsystem.h"
#include "HeapCensusSubsystem.generated.h"

class UCanvas;

/**
 * Fired once when a class first reaches the Leaking verdict - not once per census.
 *
 * Bind it to put a marker in your own telemetry, or to take a screenshot, or to write the report. HeapCensus
 * deliberately does not send anything anywhere.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FHeapCensusClassGrowingSignature,
	const FString&, ClassName, float, SlopePerMinute, EHeapVerdict, Verdict);

/**
 * The Blueprint-facing face of the census, and the thing that draws the counter box.
 *
 * An engine subsystem, not a world subsystem, and that is the point: a leak is a thing that survives a map
 * change, and the interesting question is often exactly whether the count came back down after one. A census
 * that was torn down and rebuilt with the world would lose the history that makes that question answerable.
 *
 * The subsystem owns none of the measuring. FHeapCensusRecorder does that, from the moment the module loads
 * at PreDefault; this class arrives later and finds the census already running.
 */
UCLASS(DisplayName = "HeapCensus")
class HEAPCENSUS_API UHeapCensusSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/** The one HeapCensus subsystem, or null before the engine is up. */
	static UHeapCensusSubsystem* Get();

	// --------------------------------------------------------------------------------------------------
	// The census
	// --------------------------------------------------------------------------------------------------

	/**
	 * Walk the object array now, out of turn, and return the resulting summary.
	 *
	 * The walk is the one expensive thing this plugin does - see the comment on FHeapCensusRecorder's walk -
	 * so calling this every frame defeats the entire design. It is here for the moment before and the moment
	 * after something you want to measure.
	 */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	FHeapCensusSummary Sample();

	/** The last summary, without walking anything. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	FHeapCensusSummary GetSummary() const;

	/** The N classes at the top of the given order. N of 0 or less returns the whole table. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	TArray<FHeapClassEntry> GetTop(int32 N = 5, EHeapSort Sort = EHeapSort::Growth) const;

	/** What the collection costs: the last run, the average, the worst, and how far apart they are. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	FHeapGCStats GetGCStats() const;

	/** Live UObjects at the last census, class default objects excluded. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	int32 GetTotalObjects() const;

	/** True when at least one class is Leaking. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	bool IsLeaking() const;

	/** Throw the window and the collection statistics away and start measuring again. */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	void Reset();

	/**
	 * Write the census as JSON.
	 *
	 * An empty path means the one from the project settings, which is the same file Heap.Gate writes, so a
	 * developer and a build server end up looking at the same document.
	 */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus", meta = (AdvancedDisplay = "Path"))
	bool WriteReport(const FString& Path);

	/** Write the whole table to the log - every class, not just the five the counter box shows. */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	void DumpToLog();

	/** Objects per minute at or above which a class is called Leaking. */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	void SetGrowthThreshold(float ThresholdPerMinute);

	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	float GetGrowthThreshold() const;

	/** Show or hide the counter box. */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	void SetShowCounterBox(bool bInShow);

	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	bool IsShowingCounterBox() const;

	/**
	 * Draw the counter box on a canvas.
	 *
	 * AHeapCensusHUD calls this for you. Call it from your own AHUD::DrawHUD instead if your project already
	 * has a HUD class - that is one line, and it means nobody has to choose between their HUD and this one.
	 */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	void DrawCounterBox(UCanvas* Canvas);

	/** Fired once when a class first reaches the Leaking verdict. */
	UPROPERTY(BlueprintAssignable, Category = "HeapCensus")
	FHeapCensusClassGrowingSignature OnClassGrowing;

private:
	void HandleClassGrowing(const FString& ClassName, float SlopePerMinute, EHeapVerdict Verdict);

	FDelegateHandle GrowingHandle;
};
