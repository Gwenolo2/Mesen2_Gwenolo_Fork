using Avalonia;
using Mesen.Interop;
using Mesen.Utilities;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading.Tasks;

namespace Mesen.Debugger.Utilities
{
	public static class HdPackCopyHelper
	{
		public static bool IsActionAllowed(MemoryType type)
		{
			return type switch {
				MemoryType.NesPpuMemory => true,
				MemoryType.NesChrRam => true,
				MemoryType.NesChrRom => true,
				MemoryType.SnesVideoRam => true, // <-- Ajout SNES
				MemoryType.GbVideoRam => true,   // <-- Ajout Game Boy
				_ => false,
			};
		}

		//--------------------------------------------------------------------------------------------------------------
		public static string ToHdPackFormat(AddressInfo tileAddr, UInt32[] rawPalette, int paletteIndex, bool forSprite)
		{
			StringBuilder sb = new StringBuilder();

			if(tileAddr.Type == MemoryType.NesPpuMemory) {
				tileAddr = DebugApi.GetAbsoluteAddress(tileAddr);
			}

			// =========================================================
			// BRANCHE SNES
			// =========================================================
			if(tileAddr.Type == MemoryType.SnesVideoRam) {
				ulong hash = 0;

				// En SNES 4BPP (Sprites et majorité des BGs), une tuile fait 32 octets (16 mots).
				// L'API C# lit les octets en Little Endian, ce qui reproduit exactement 
				// le comportement de ton code C++ (word & 0xFF puis word >> 8).
				for(uint i = 0; i < 32; i++) {
					byte b = DebugApi.GetMemoryValue(MemoryType.SnesVideoRam, (uint)(tileAddr.Address + i));
					hash = (hash * 31) + b;
				}

				// 1. Ajout du MemoryHash (Format Hexadécimal sur 16 caractères)
				sb.Append(hash.ToString("X16"));

				// 2. Ajout de la Palette (Format Index)
				// En SNES, la palette OAM commence à 8. Les BGs commencent à 0.
				int snesPalIndex = paletteIndex + (forSprite ? 8 : 0);
				sb.Append(",");
				sb.Append(snesPalIndex.ToString("X2"));

				return sb.ToString();
			}
			// =========================================================

			// --- Code NES d'origine ---
			switch(tileAddr.Type) {
				case MemoryType.NesChrRom: sb.Append((tileAddr.Address / 16).ToString("X2")); break;
				case MemoryType.NesChrRam:
					for(int i = 0; i < 16; i++) {
						sb.Append(DebugApi.GetMemoryValue(MemoryType.NesChrRam, (uint)(tileAddr.Address + i)).ToString("X2"));
					}
					break;

				default:
					return "";
			}

			if(forSprite) {
				sb.Append(",FF");
				for(int i = 1; i < 4; i++) {
					sb.Append(rawPalette[(paletteIndex + 4) * 4 + i].ToString("X2"));
				}
			} else {
				sb.Append(",");
				sb.Append(rawPalette[0].ToString("X2")); //Always use color 0 for palette index 0
				for(int i = 1; i < 4; i++) {
					sb.Append(rawPalette[paletteIndex * 4 + i].ToString("X2"));
				}
			}

			return sb.ToString();
		}
		//--------------------------------------------------------------------------------------------------------------------------------------
		// ====================================================================
		// GAME BOY / GBC : hash des 16 octets de la tile (identique au builder, doc 30 D8)
		// Palette : GBC = index 0-7 (BG) ou 8-15 (OBJ) ; DMG = registre BGP (BG) ou 256 + OBP0/OBP1 (OBJ)
		// ====================================================================
		public static ulong GbTileHash(int tileAddress)
		{
			ulong hash = 0;
			for(uint i = 0; i < 16; i++) {
				byte b = DebugApi.GetMemoryValue(MemoryType.GbVideoRam, (uint)(tileAddress + i));
				hash = (hash * 31) + b;
			}
			return hash;
		}

		public static int GbPaletteIndex(bool cgb, bool forSprite, int attrPalette, GbPpuState state)
		{
			if(cgb) {
				return forSprite ? 8 + attrPalette : attrPalette;
			}
			if(forSprite) {
				return 256 + (attrPalette == 1 ? state.ObjPalette1 : state.ObjPalette0);
			}
			return state.BgPalette;
		}

		public static string GbTileLine(int tileAddress, int palette, string image, int x, int y)
		{
			return "<tile>" + GbTileHash(tileAddress).ToString("X16") + "," + palette + "," + image + "," + x + "," + y;
		}

		public static void CopyToHdPackFormat(int address, MemoryType memoryType, UInt32[] palette, int paletteIndex, bool forSprite, bool isLargeSprite = false)
		{
			AddressInfo addr = new AddressInfo() { Address = address, Type = memoryType };
			string hdPackTile = HdPackCopyHelper.ToHdPackFormat(addr, palette, paletteIndex, forSprite);

			if(isLargeSprite && hdPackTile.Length > 0) {
				// La NES lit 16 octets par tuile, la SNES en lit 32 (pour du 4BPP)
				int offset = (memoryType == MemoryType.SnesVideoRam) ? 32 : 16;
				addr.Address += offset;

				hdPackTile += Environment.NewLine + HdPackCopyHelper.ToHdPackFormat(addr, palette, paletteIndex, forSprite);
			}

			if(hdPackTile.Length > 0) {
				ApplicationHelper.GetMainWindow()?.Clipboard?.SetTextAsync(hdPackTile);
			}
		}
		//--------------------------------------------------


	}
}
