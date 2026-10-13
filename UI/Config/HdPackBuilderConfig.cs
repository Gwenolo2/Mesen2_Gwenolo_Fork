using Mesen.Interop;
using ReactiveUI.Fody.Helpers;
using System.Runtime.InteropServices;
using System;

namespace Mesen.Config
{
	public class HdPackBuilderConfig : BaseConfig<HdPackBuilderConfig>
	{
		public ScaleFilterType FilterType { get; set; } = ScaleFilterType.Prescale;
		public UInt32 Scale { get; set; } = 1;
		public UInt32 ChrRamBankSize { get; set; } = 0x1000;
		public bool UseLargeSprites { get; set; } = false;
		public bool SortByUsageFrequency { get; set; } = true;
		public bool GroupBlankTiles { get; set; } = true;
		public bool IgnoreOverscan { get; set; } = false;

		// For SNES
		public bool DisableM7TextureDump { get; set; } = false;
		public bool IgnorePlainTiles { get; set; } = false;
		public bool GenerateSpritesPerFrame { get; set; } = true;
		public bool PreventHMirroredSpritesGrabbing { get; set; } = true;
		public bool GenerateHdPackMetadata { get; set; } = true;

		// ====================================================================
		// LOGIQUE D'EXCLUSION MUTUELLE (UpdateIfExists vs BuildFromPngFolder)
		// ====================================================================
		private bool _updateIfExists = true;
		public bool UpdateIfExists
		{
			get => _updateIfExists;
			set
			{
				_updateIfExists = value;
				if(value) {
					_buildFromPngFolder = false; // Si j'active l'Update, je désactive le Smart Rebuild
				}
			}
		}

		private bool _buildFromPngFolder = false;
		public bool BuildFromPngFolder
		{
			get => _buildFromPngFolder;
			set
			{
				_buildFromPngFolder = value;
				if(value) {
					_updateIfExists = false; // Si j'active le Smart Rebuild, je désactive l'Update
				}
			}
		}
		// ====================================================================

		public HdPackBuilderOptions ToInterop(string saveFolder, string inputPngFolder = "")
		{
			return new HdPackBuilderOptions() {
				SaveFolder = saveFolder,
				//InputPngFolder = inputPngFolder, 
				FilterType = FilterType,
				Scale = Scale,
				ChrRamBankSize = ChrRamBankSize,
				UseLargeSprites = UseLargeSprites,
				SortByUsageFrequency = SortByUsageFrequency,
				GroupBlankTiles = GroupBlankTiles,
				IgnoreOverscan = IgnoreOverscan,
				DisableM7TextureDump = DisableM7TextureDump,
				IgnorePlainTiles = IgnorePlainTiles,
				GenerateSpritesPerFrame = GenerateSpritesPerFrame,
				PreventHMirroredSpritesGrabbing = PreventHMirroredSpritesGrabbing,
				PreventVMirroredSpritesGrabbing = true,
				GenerateHdPackMetadata = GenerateHdPackMetadata,
				UpdateIfExists = UpdateIfExists,
				BuildFromPngFolder = BuildFromPngFolder // NOUVEAU
			};
		}
	}

	public enum ScaleFilterType
	{
		xBRZ = 0,
		HQX = 1,
		Scale2x = 2,
		_2xSai = 3,
		Super2xSai = 4,
		SuperEagle = 5,
		Prescale = 6,
		LcdGrid = 7,
	}

	public struct HdPackBuilderOptions
	{
		[MarshalAs(UnmanagedType.LPUTF8Str)] public string SaveFolder;

		// Le dossier où l'on va scanner les PNG pour le retro scan des PNG 
		//[MarshalAs(UnmanagedType.LPUTF8Str)] public string InputPngFolder;

		public ScaleFilterType FilterType;
		public UInt32 Scale;
		public UInt32 ChrRamBankSize;

		[MarshalAs(UnmanagedType.I1)] public bool UseLargeSprites;
		[MarshalAs(UnmanagedType.I1)] public bool SortByUsageFrequency;
		[MarshalAs(UnmanagedType.I1)] public bool GroupBlankTiles;
		[MarshalAs(UnmanagedType.I1)] public bool IgnoreOverscan;

		// For SNES HD PACK Builder:
		[MarshalAs(UnmanagedType.I1)] public bool DisableM7TextureDump;
		[MarshalAs(UnmanagedType.I1)] public bool IgnorePlainTiles;
		[MarshalAs(UnmanagedType.I1)] public bool GenerateSpritesPerFrame;
		[MarshalAs(UnmanagedType.I1)] public bool PreventHMirroredSpritesGrabbing;
		[MarshalAs(UnmanagedType.I1)] public bool PreventVMirroredSpritesGrabbing;  
		[MarshalAs(UnmanagedType.I1)] public bool GenerateHdPackMetadata;
		[MarshalAs(UnmanagedType.I1)] public bool UpdateIfExists;

		[MarshalAs(UnmanagedType.I1)] public bool BuildFromPngFolder;
	}
}