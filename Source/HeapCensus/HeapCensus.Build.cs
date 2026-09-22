// Copyright 2026 Silvan Teufel. All Rights Reserved.

using UnrealBuildTool;

public class HeapCensus : ModuleRules
{
	public HeapCensus(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// One runtime module, and deliberately no editor module.
		//
		// The growth this plugin exists to catch is growth a developer never sees: it takes forty minutes of
		// play to become visible, and by then nobody has the editor open any more. So everything here has to
		// survive cooking and has to work in a packaged Shipping build - which is also why the counter box is
		// drawn on UCanvas from an AHUD rather than in UMG. A widget tree would have to be cooked, referenced
		// and kept alive by the project; a Canvas box works in a level that contains nothing but a floor.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",

			// CoreUObject: everything this plugin measures lives here. The object array it walks
			// (FThreadSafeObjectIterator over GUObjectArray), and the two garbage-collection hooks it times
			// the collection with.
			"CoreUObject",

			"Engine",
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// RenderCore: GWhiteTexture, the background tile behind the counter box.
			"RenderCore",

			// Json: Heap.Gate writes Saved/HeapCensus/report.json for the build server.
			"Json",
			"JsonUtilities",
		});

		// Deliberately NOT here: UnrealEd, UMG, Slate.
		//
		// Memory Insights already exists and is a better recorder than anything this plugin could be. What it
		// is not is a number on the screen while somebody plays the build they will ship, in a session nobody
		// planned to profile. That is the whole gap, and closing it means not linking a single editor-only
		// module.
	}
}
