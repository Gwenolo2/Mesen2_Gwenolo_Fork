using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Mesen.Interop
{
	public static class SnesInterop
	{
		[DllImport("MesenCore", CallingConvention = CallingConvention.Cdecl)]
		public static extern ulong SnesGetHdTileHash(uint tileIndex, byte bpp);

		[DllImport("MesenCore", CallingConvention = CallingConvention.Cdecl)]
		public static extern uint SnesGetHdMode7Hash(bool forTileset);
	}
}