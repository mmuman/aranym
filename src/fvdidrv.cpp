/*
 * $Header$
 *
 * STanda 2001
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"

#include "hostscreen.h"
#include "videl.h"

#include "fvdidrv.h"
#include "hardware.h"

#define DEBUG 0
#include "debug.h"

#include <new>     // Johan Klockars
#include <cstring> // Johan Klockars

// this serves for debugging the color palette code
#undef DEBUG_DRAW_PALETTE


// from host.cpp
extern HostScreen hostScreen;


// The Atari structures offsets
#define MFDB_ADDRESS                0
#define MFDB_WIDTH                  4
#define MFDB_HEIGHT                 6 
#define MFDB_WDWIDTH                8
#define MFDB_STAND                 10
#define MFDB_NPLANES               12

#if SDL_BYTEORDER == SDL_BIG_ENDIAN

#define put_dtriplet(address, data) \
{ \
	WriteInt8((address), ((data) >> 16) & 0xff); \
	WriteInt8((address) + 1, ((data) >> 8) & 0xff); \
	WriteInt8((address) + 2, (data) & 0xff); \
}

#define get_dtriplet(address) \
	((ReadInt8((address)) << 16) | (ReadInt8((address) + 1) << 8) | ReadInt8((address) + 2))

#else

#define put_dtriplet(address, data) \
{ \
	WriteInt8((address), (data) & 0xff); \
	WriteInt8((address) + 1, ((data) >> 8) & 0xff); \
	WriteInt8((address) + 2, ((data) >> 16) & 0xff); \
}

#define get_dtriplet(address) \
	((ReadInt8((address) + 2) << 16) | (ReadInt8((address) + 1) << 8) | ReadInt8((address)))

#endif // SDL_BYTEORDER == SDL_BIG_ENDIAN



// The polygon code needs some arrays of unknown size
// These routines and members are used so that no unnecessary allocations are done
inline bool FVDIDriver::AllocIndices(int n)
{
	if (n > index_count) {
		D2(bug("More indices %d->%d\n", index_count, n));
		int count = n * 2;		// Take a few extra right away
		int16* tmp = new(std::nothrow) int16[count];
		if (!tmp) {
			count = n;
			tmp = new(std::nothrow) int16[count];
		}
		if (tmp) {
			delete[] alloc_index;
			alloc_index = tmp;
			index_count = count;
		}
	}

	return index_count >= n;
}

inline bool FVDIDriver::AllocCrossings(int n)
{
	if (n > crossing_count) {
		D2(bug("More crossings %d->%d\n", crossing_count, n));
		int count = n * 2;		// Take a few extra right away
		int16* tmp = new(std::nothrow) int16[count];
		if (!tmp) {
			count = (n * 3) / 2;	// Try not so many extra
			tmp = new(std::nothrow) int16[count];
		}
		if (!tmp) {
			count = n;		// This is going to be slow if it goes on...
			tmp = new(std::nothrow) int16[count];
		}
		if (tmp) {
			std::memcpy(tmp, alloc_crossing, crossing_count * sizeof(*alloc_crossing));
			delete[] alloc_crossing;
			alloc_crossing = tmp;
			crossing_count = count;
		}
	}

	return crossing_count >= n;
}

inline bool FVDIDriver::AllocPoints(int n)
{
	if (n > point_count) {
		D2(bug("More points %d->%d", point_count, n));
		int count = n * 2;		// Take a few extra right away
		int16* tmp = new(std::nothrow) int16[count * 2];
		if (!tmp) {
			count = n;
			tmp = new(std::nothrow) int16[count * 2];
		}
		if (tmp) {
			delete[] alloc_point;
			alloc_point = tmp;
			point_count = count;
		}
	}

	return point_count >= n;
}

// A helper class to make it possible to access
// points in a nicer way in fillPoly.
class Points {
  public:
	explicit Points(int16* vector_) : vector(vector_) { }
	~Points() { }
	int16* operator[](int n) { return &vector[n * 2]; }
  private:
	int16* vector;
};


int32 FVDIDriver::get_par(M68kRegisters *r, int n)
{
	return ReadInt32(r->a[7] + 4 + n * 4);
}


void FVDIDriver::dispatch(M68kRegisters *r)
{
	// Thread safety patch (remove it once the fVDI screen output is in the main thread)
	hostScreen.lock();

	videl.setRendering(false);

	uint32 fncode = ReadInt32(r->a[7] + 4);
	switch (fncode) {
		// NEEDED functions

		case 1: // read_pixel
			r->d[0] = getPixel((memptr)get_par(r, 1),		// vwk
			                   (memptr)get_par(r, 2),		// src MFDB*
			                   get_par(r, 3), get_par(r, 4));	// x, y
			break;

		case 2: // write_pixel
			r->d[0] = putPixel((memptr)get_par(r, 1),		// vwk
			                   (memptr)get_par(r, 2),		// dst MFDB*
			                   get_par(r, 3), get_par(r, 4),	// x, y
			                                                	// table*, table length/type (0 - coordinates)
			                   get_par(r, 5));			// color
			break;

		case 3: // mouse_draw
			r->d[0] = drawMouse((memptr)get_par(r, 1),		// wk
			                    get_par(r, 2), get_par(r, 3),	// x, y
			                    get_par(r, 4),			// mask*
			                                  			// mode (0 - move, 1 - hide, 2 - show)
			                                  			// These are only valid when not mode
			                    get_par(r, 5),			//   data*
			                    get_par(r, 6), get_par(r, 7),	//   hot_x, hot_y
			                    get_par(r, 8),			//   color
			                    get_par(r, 9));			//   type
			break;


		// SPEEDUP functions

		case 4: // expand_area
			r->d[0] = expandArea((memptr)get_par(r, 1),		// vwk
			                     (memptr)get_par(r, 2),		// src MFDB*
			                     get_par(r, 3), get_par(r, 4),	// sx, sy
			                     (memptr)get_par(r, 5),		// dest MFDB*
			                     get_par(r, 6), get_par(r, 7),	// dx, dy
			                     get_par(r, 8),			// width
			                     get_par(r, 9),			// height
			                     get_par(r, 10),			// logical operation
			                     get_par(r, 11));			// bgColor fgColor
			break;

		case 5: // fill_area
			r->d[0] = fillArea((memptr)get_par(r, 1),		// vwk
			                   get_par(r, 2), get_par(r, 3),	// x, y
			                                                	// table*, table length/type (0 - y/x1/x2 spans)
			                   get_par(r, 4), get_par(r, 5),	// width, height
			                   (memptr)get_par(r, 6),		// pattern*
			                   get_par(r, 7),			// bgColor fgColor
			                   get_par(r, 8),			// mode
			                   get_par(r, 9));			// interior style
			break;

		case 6: // blit_area
			r->d[0] = blitArea((memptr)get_par(r, 1),		// vwk
			                   (memptr)get_par(r, 2),		// src MFDB*
			                   get_par(r, 3), get_par(r, 4),	// sx, sy
			                   (memptr)get_par(r, 5),		// dest MFDB*
			                   get_par(r, 6), get_par(r, 7),	// dx, dy
			                   get_par(r, 8),			// width
			                   get_par(r, 9),			// height
			                   get_par(r, 10));			// logical operation
			break;

		case 7: // drawLine:
			r->d[0] = drawLine((memptr)get_par(r, 1),		// vwk
			                   get_par(r, 2), get_par(r, 3),	// x1, y1
			                                                	// table*, table length/type (0 - coordinate pairs, 1 - pairs + moves)
			                   get_par(r, 4), get_par(r, 5),	// x2, y2
			                                                	// move point count, move index*
			                   get_par(r, 6) & 0xffff,		// pattern
			                   get_par(r, 7),			// bgColor fgColor
			                   get_par(r, 8) & 0xffff,		// logical operation
			                   (memptr)get_par(r, 9));		// clip rectangle* (0 or long[4])
			break;

		case 8: // fill_polygon
			r->d[0] = fillPoly((memptr)get_par(r, 1),		// vwk
			                   (memptr)get_par(r, 2),		// points*
			                   get_par(r, 3),			// point count
			                   (memptr)get_par(r, 4),		// index*
			                   get_par(r, 5),			// index count
			                   (memptr)get_par(r, 6),		// pattern*
			                   get_par(r, 7),			// bgColor fgColor
			                   get_par(r, 8),			// logic operation
			                   get_par(r, 9),			// interior style
			                   (memptr)get_par(r, 10));		// clip rectangle
			break;

		case 9: // setColor:
			setColor(get_par(r, 1), get_par(r, 2), get_par(r, 3), get_par(r, 4));
			break;

		case 10: // setResolution:
			setResolution(get_par(r, 1), get_par(r, 2), get_par(r, 3), get_par(r, 4));
			break;

		case 15: // getVideoramAddress:
#ifdef FIXED_VIDEORAM
			r->d[0] = ARANYMVRAMSTART;
#else
			r->d[0] = VideoRAMBase;
#endif
			D(bug("fVDI: getVideoramAddress: %#lx", r->d[0]));
			break;

		case 20: // debug_aranym:
			bug("fVDI: DEBUG %d", get_par(r, 1));
			break;

			// not implemented functions
		default:
			D(bug("fVDI: Unknown %d", fncode));
			r->d[0] = 1;
	}

#ifdef DEBUG_DRAW_PALETTE
	uint16 ptrn[16] = {
		0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff
	};
	for(int i = 0; i < 16; i++)
		hostScreen.fillArea(i << 4, hostScreen.getHeight() - 16, 15, 16, ptrn,
		                    hostScreen.getBpp() > 1 ? hostScreen.getPaletteColor(i) : i);
#endif  // DEBUG_DRAW_PALETTE


	// Thread safety patch (remove it once the fVDI screen output is in the main thread)
	hostScreen.unlock();
}


/**
 * Set a coloured pixel.
 *
 * c_write_pixel(Virtual *vwk, MFDB *mfdb, long x, long y, long colour)
 * write_pixel
 * In:   a1  VDI struct, destination MFDB
 *   d0  colour
 *   d1  x or table address
 *   d2  y or table length (high) and type (low)
 * XXX: ?
 *
 * This function has two modes:
 *   - single pixel
 *   - table based multi pixel (special mode 0 (low word of 'y'))
 *
 * Note that a1 does not point to the VDI struct, but to a place in memory
 * where the VDI struct pointer can be found. Four bytes beyond that address
 * is a pointer to the destination MFDB.
 *
 * As usual, only the first one is necessary, and a return with d0 = -1
 * signifies that a special mode should be broken down to the basic one.
 *
 * Since an MFDB is passed, the destination is not necessarily the screen.
 **/
int FVDIDriver::putPixel(memptr vwk, memptr dst, int32 x, int32 y, uint32 color)
{
	if (vwk & 1)
		return 0;

	if (!dst) {
		// To screen
		if (!hostScreen.renderBegin())
			return 1;
		hostScreen.putPixel((int16)x, (int16)y, color);
		hostScreen.renderEnd();
		hostScreen.update((int16)x, (int16)y, 1, 1, true);
	} else {
		D(bug("fVDI: %s %d,%d (%d)", "write_pixel no screen", (uint16)x, (uint16)y, (uint32)color));
		uint16 planes = ReadInt16(dst + MFDB_NPLANES);
		uint32 row_address = ReadInt32(dst + MFDB_ADDRESS) +
		                     ReadInt16(dst + MFDB_WDWIDTH) * 2 * planes * y;
		switch (planes) {
		case 8:
			WriteInt8(row_address + x, color);
			break;
		case 16:
			WriteInt16(row_address + x * 2, color);
			break;
		case 24:
			WriteInt8(row_address + x * 3, (color >> 16) & 0xff);
			WriteInt8(row_address + x * 3 + 1, (color >> 8) & 0xff);
			WriteInt8(row_address + x * 3 + 2, color & 0xff);
			break;
		case 32:
			WriteInt32(row_address + x * 4, color);
			break;
		default:
			break;
		}
	}

	return 1;
}

/**
 * Get a coloured pixel.
 *
 * c_read_pixel(Virtual *vwk, MFDB *mfdb, long x, long y)
 * read_pixel
 * In:  a1  VDI struct, source MFDB
 *  d1  x
 *  d2  y
 *
 * Only one mode here.
 *
 * Note that a1 does not point to the VDI struct, but to a place in memory
 * where the VDI struct pointer can be found. Four bytes beyond that address
 * is a pointer to the destination MFDB.
 *
 * Since an MFDB is passed, the source is not necessarily the screen.
 **/
uint32 FVDIDriver::getPixel(memptr vwk, memptr src, int32 x, int32 y)
{
	uint32 color = 0;

	if (!src) {
		if (!hostScreen.renderBegin())
			return 0;
		color = hostScreen.getPixel((int16)x, (int16)y);
		hostScreen.renderEnd();
	} else {
		D(bug("fVDI: %s %d,%d (%d)", "read_pixel no screen", (uint16)x, (uint16)y, (uint32)color));
		uint16 planes = ReadInt16(src + MFDB_NPLANES);
		uint32 row_address = ReadInt32(src + MFDB_ADDRESS) +
		                     ReadInt16(src + MFDB_WDWIDTH) * 2 * planes * y;
		switch (planes) {
		case 8:
			color = ReadInt8(row_address + x);
			break;
		case 16:
			color = ReadInt16(row_address + x * 2);
			break;
		case 24:
			color = ((uint32)ReadInt8(row_address + x * 3) << 16) +
			        ((uint32)ReadInt8(row_address + x * 3 + 1) << 8) +
			        (uint32)ReadInt8(row_address + x * 3 + 2);
			break;
		case 32:
			color = ReadInt32(row_address + x * 4);
			break;
		default:
			color = 0xffffffff;
			break;
		}
	}

	return color;
}


/**
 * Set palette colour (hooked into the c_set_colors driver function by STanda)
 *
 * set_color_hook
 *  4(a7)   paletteIndex
 *  8(a7)   red component byte value
 *  10(a7)  green component byte value
 *  12(a7)  blue component byte value
 **/
void FVDIDriver::setColor(uint32 paletteIndex, uint32 red, uint32 green, uint32 blue)
{
	D(bug("fVDI: setColor: %03d - %3d,%3d,%3d - %02x,%02x,%02x", paletteIndex, red, green, blue, (uint8)(((red << 8) - 1) / 1000), (uint8)(((green << 8) - 1) / 1000), (uint8)(((blue << 8) - 1) / 1000)));

	if (hostScreen.getBpp() == 2) // 5+6+5 rounding needed
		hostScreen.setPaletteColor(paletteIndex,
		                           ((red * ((1L << 5) - 1) + 500L) / 1000) << 3,
		                           ((green * ((1L << 6) - 1) + 500L) / 1000) << 2,
		                           ((blue * ((1L << 5) - 1) + 500L) / 1000) << 3);
	else // 8+8+8 graphics
		hostScreen.setPaletteColor(paletteIndex,
		                           ((red * ((1L << 8) - 1) + 500L) / 1000),
		                           ((green * ((1L << 8) - 1) + 500L) / 1000),
		                           ((blue * ((1L << 8) - 1) + 500L) / 1000));

	/*
	  (uint8)((color >> 8) & 0xf8),
	  (uint8)((color >> 3) & 0xfc),
	  (uint8)(color & 0x1f) << 3);
	*/
}


void FVDIDriver::setResolution(int32 width, int32 height, int32 depth, int32 freq)
{
	D(bug("fVDI: setResolution: %dx%dx%d@%d", width, height, depth, freq));
	hostScreen.setWindowSize(width, height, depth > 8 ? depth : 8);
}


/*************************************************************************************************/
/**************************************************************************************************
 * 'Wanted' functions
 * ------------------
 * While fVDI can make do without these, it will be very slow.
 * Note that the fallback mechanism can be used for uncommon operations
 * without sacrificing speed for common ones. At least during development,
 * it is also possible to simply ignore some things, such as a request for
 * colour, patterns, or a specific effect.
**************************************************************************************************/
/*************************************************************************************************/

/**
 * Draw the mouse
 *
 * Draw a coloured line between two points.
 *
 * c_mouse_draw(Workstation *wk, long x, long y, Mouse *mouse)
 * mouse_draw
 * In:  a1  Pointer to Workstation struct
 *  d0/d1   x,y
 *  d2  0 - move shown  1 - move hidden  2 - hide  3 - show  >3 - change shape (pointer to mouse struct)
 *
 * Unlike all the other functions, this does not receive a pointer to a VDI
 * struct, but rather one to the screen's workstation struct. This is
 * because the mouse handling concerns the screen as a whole (and the
 * routine is also called from inside interrupt routines).
 *
 * The Mouse structure pointer doubles as a mode variable. If it is a small
 * number, the mouse's state is supposed to change somehow, while a large
 * number is a pointer to a new mouse shape.
 *
 * This is currently not a required function, but it probably should be.
 * The fallback handling is not done in the usual way, and to make it
 * at least somewhat usable, the mouse pointer is reduced to 4x4 pixels.
 *
 * typedef struct Fgbg_ {
 *   short background;
 *   short foreground;
 * } Fgbg;
 *
 * typedef struct Mouse_ {
 * 0  short type;
 * 2   short hide;
 *   struct position_ {
 * 4   short x;
 * 6   short y;
 *   } position;
 *   struct hotspot_ {
 * 8   short x;
 * 10   short y;
 *   } hotspot;
 * 12  Fgbg colour;
 * 16  short mask[16];
 * 48  short data[16];
 * 80  void *extra_info;
 * } Mouse;
 **/
extern "C" {
#if DEBUG > 0
	static void getBinary(uint16 data, char *buffer)
	{
		for(uint16 i = 0; i <= 15; i++) {
			buffer[i] = (data & 1) ? '1' : ' ';
			data >>= 1;
		}
		buffer[16] = '\0';
	}
#endif
	static uint16 reverse_bits(uint16 data)
	{
		uint16 res = 0;
		for(uint16 i = 0; i <= 15; i++)
			res |= ((data >> i) & 1) << (15 - i);
		return res;
	}
}


void FVDIDriver::restoreMouseBackground()
{
	int16 x = Mouse.storage.x;
	int16 y = Mouse.storage.y;

	if (!hostScreen.renderBegin())
		return;

	for(uint16 i = 0; i < Mouse.storage.height; i++)
		for(uint16 j = 0; j < Mouse.storage.width; j++)
			hostScreen.putPixel(x + j, y + i, Mouse.storage.background[i][j]);

	hostScreen.renderEnd();
	hostScreen.update(x, y, Mouse.storage.width, Mouse.storage.height, true);
}


void FVDIDriver::saveMouseBackground(int16 x, int16 y, int16 width, int16 height)
{
	D2(bug("fVDI: saveMouseBackground: %d,%d,%d,%d", x, y, width, height));

	if (!hostScreen.renderBegin())
		return;

	for(uint16 i = 0; i < height; i++)
		for(uint16 j = 0; j < width; j++) {
			Mouse.storage.background[i][j] = hostScreen.getPixel(x + j, y + i);
		}

	hostScreen.renderEnd();

	Mouse.storage.x = x;
	Mouse.storage.y = y;
	Mouse.storage.height = height;
	Mouse.storage.width = width;
}


int FVDIDriver::drawMouse(memptr wrk, int32 x, int32 y, uint32 mode, uint32 data,
                          uint32 hot_x, uint32 hot_y, uint32 color, uint32 mouse_type)
{
	D2(bug("fVDI: mouse mode: %x", mode));

	switch (mode) {
	case 0:  // move shown
		restoreMouseBackground();
		break;
	case 1:  // move hidden
		return 1;
	case 2:  // hide
		restoreMouseBackground();
		return 1;
	case 3:  // show
		break;

	default: // change pointer shape
		memptr fPatterAddress = (memptr)mode;
		for(uint16 i = 0; i < 32; i += 2)
			Mouse.mask[i >> 1] = reverse_bits(ReadInt16(fPatterAddress + i));
		fPatterAddress  = (memptr)data;
		for(uint16 i = 0; i < 32; i += 2)
			Mouse.shape[i >> 1] = reverse_bits(ReadInt16(fPatterAddress + i));

		Mouse.hotspot.x = hot_x;
		Mouse.hotspot.y = hot_y;
		Mouse.storage.color.foreground = (int16)(color & 0xffff);
		Mouse.storage.color.background = (int16)(color >> 16);
		if (hostScreen.getBpp() > 1) {
			Mouse.storage.color.foreground = hostScreen.getPaletteColor(Mouse.storage.color.foreground);
			Mouse.storage.color.background = hostScreen.getPaletteColor(Mouse.storage.color.background);
		}

#if DEBUG > 1
		char buffer[30];
		for(uint16 i = 0; i <= 15; i++) {
			getBinary(Mouse.mask[i], buffer);
			D2(bug("fVDI: apm:%s", buffer));
		}
		for(uint16 i = 0; i <= 15; i++) {
			getBinary(Mouse.shape[i], buffer);
			D2(bug("fVDI: apd:%s", buffer));
		}
#endif // DEBUG == 1

		return 1;
	}

	// handle the mouse hotspot point
	x -= Mouse.hotspot.x;
	y -= Mouse.hotspot.y;

	// roll the pattern properly
	uint16 mm[16];
	uint16 md[16];
	uint16 shift = x & 0xf;
	for(uint16 i = 0; i <= 15; i++) {
		mm[(i + (y & 0xf)) & 0xf] = (Mouse.mask[i]  << shift) | ((Mouse.mask[i]  >> (16 - shift)) & ((1 << shift) - 1));
		md[(i + (y & 0xf)) & 0xf] = (Mouse.shape[i] << shift) | ((Mouse.shape[i] >> (16 - shift)) & ((1 << shift) - 1));
	}

	// beware of the edges of the screen
	int16 w, h;

	if (x < 0) {
		w = 16 + x;
		x = 0;
	} else {
		w = 16;
		if ((int16)x + 16 >= (int32)hostScreen.getWidth())
			w = hostScreen.getWidth() - (int16)x;
	}
	if (y < 0) {
		h = 16 + y;
		y = 0;
	} else {
		h = 16;
		if ((int16)y + 16 >= (int32)hostScreen.getHeight())
			h = hostScreen.getHeight() - (int16)y;
	}

	D2(bug("fVDI: mouse x,y: %d,%d,%d,%d (%x,%x)", x, y, w, h,
	   Mouse.storage.color.background, Mouse.storage.color.foreground));

	// draw the mouse
	if (!hostScreen.renderBegin())
		return 1;

	saveMouseBackground(x, y, w, h);

	hostScreen.fillArea(x, y, w, h, mm, Mouse.storage.color.background);
	hostScreen.fillArea(x, y, w, h, md, Mouse.storage.color.foreground);

	hostScreen.renderEnd();
	hostScreen.update((uint16)x, (uint16)y, w, h, true);

	return 1;
}


/**
 * Expand a monochrome area to a coloured one.
 *
 * c_expand_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
 *                             MFDB *dst, long dst_x, long dst_y,
 *                             long w, long h, long operation, long colour)
 * expand_area
 * In:  a1  VDI struct, destination MFDB, VDI struct, source MFDB
 *  d0  height and width to move (high and low word)
 *  d1-d2   source coordinates
 *  d3-d4   destination coordinates
 *  d6  background and foreground colour
 *  d7  logic operation
 *
 * Only one mode here.
 *
 * Note that a1 does not point to the VDI struct, but to a place in memory
 * where the VDI struct pointer can be found. Four bytes beyond that address
 * is a pointer to the destination MFDB, and then comes a VDI struct
 * pointer again (the same) and a pointer to the source MFDB.
 *
 * Since MFDBs are passed, the screen is not necessarily involved.
 *
 * A return with 0 gives a fallback (normally pixel by pixel drawing by
 * the fVDI engine).
 *
 * typedef struct MFDB_ {
 *   short *address;
 *   short width;
 *   short height;
 *   short wdwidth;
 *   short standard;
 *   short bitplanes;
 *   short reserved[3];
 * } MFDB;
 **/
extern "C" {
	void keypress()
	{
		SDL_Event event;
		int quit = 0;
		while (!quit) {
			while (SDL_PollEvent(&event)) {
				if (event.type == SDL_KEYDOWN) {
					quit = 1;
					break;
				}
			}
		}
	}
}

int FVDIDriver::expandArea(memptr vwk, memptr src, int32 sx, int32 sy, memptr dest, int32 dx, int32 dy,
                           int32 w, int32 h, uint32 logOp, uint32 colors)
{

	D(bug("fVDI: %s %x %d,%d:%d,%d:%d,%d (%d, %d)", "expandArea", logOp, sx, sy, dx, dy, w, h, fgColor, bgColor ));
	D2(bug("fVDI: %s %x,%x : %x,%x", "expandArea - MFDB addresses", src, dest, ReadInt32( src ),ReadInt32( dest )));

	uint16 pitch = ReadInt16(src + MFDB_WDWIDTH) * 2; // the byte width (always monochrom);
	memptr data  = ReadInt32(src + MFDB_ADDRESS) + sy * pitch; // MFDB *src->address;

	D2(bug("fVDI: %s %x, %d, %d", "expandArea - src: data address, MFDB wdwidth << 1, bitplanes", data, pitch, ReadInt16( src + MFDB_NPLANES )));
	D2(bug("fVDI: %s %x, %d, %d", "expandArea - dst: data address, MFDB wdwidth << 1, bitplanes", ReadInt32(dest), ReadInt16(dest + MFDB_WDWIDTH) * (ReadInt16(dest + MFDB_NPLANES) >> 2), ReadInt16(dest + MFDB_NPLANES)));

	uint32 fgColor = (int16)(colors & 0xffff);
	uint32 bgColor = (int16)(colors >> 16);
	if (hostScreen.getBpp() > 1) {
		fgColor = hostScreen.getPaletteColor(fgColor);
		bgColor = hostScreen.getPaletteColor(bgColor);
	}

	if (dest) {
		// mfdb->address = videoramstart

		D(bug("fVDI: expandArea M->M"));

		uint32 destPlanes  = (uint32)ReadInt16( dest + MFDB_NPLANES );
		uint32 destPitch   = ReadInt16(dest + MFDB_WDWIDTH) * destPlanes << 1; // MFDB *dest->pitch
		uint32 destAddress = ReadInt32(dest);

		switch(destPlanes) {
		case 16:
			for(uint16 j = 0; j < h; j++) {
				D2(fprintf(stderr, "fVDI: bmp:"));

				uint16 theWord = ReadInt16(data + j * pitch + ((sx >> 3) & 0xfffe));
				for(uint16 i = sx; i < sx + w; i++) {
					uint32 offset = (dx + i - sx) * 2 + (dy + j) * destPitch;
					if (i % 16 == 0)
						theWord = ReadInt16(data + j * pitch + ((i >> 3) & 0xfffe));

					D2(fprintf(stderr, "%s", ((theWord >> (15 - (i & 0xf))) & 1) ? "1" : " "));
					switch(logOp) {
					case 1:
						WriteInt16(destAddress + offset, ((theWord >> (15 - (i & 0xf))) & 1) ? fgColor : bgColor);
						break;
					case 2:
						if ((theWord >> (15 - (i & 0xf))) & 1)
							WriteInt16(destAddress + offset, fgColor);
						break;
					case 3:
						if ((theWord >> (15 - (i & 0xf))) & 1)
							WriteInt16(destAddress + offset, ~ReadInt16(destAddress + offset));
						break;
					case 4:
						if (!((theWord >> (15 - (i & 0xf))) & 1))
							WriteInt16(destAddress + offset, fgColor);
						break;
					}
				}
				D2(bug("")); //newline
			}
			break;
		case 24:
			for(uint16 j = 0; j < h; j++) {
				D2(fprintf(stderr, "fVDI: bmp:"));

				uint16 theWord = ReadInt16(data + j * pitch + ((sx >> 3) & 0xfffe));
				for(uint16 i = sx; i < sx + w; i++) {
					uint32 offset = (dx + i - sx) * 3 + (dy + j) * destPitch;
					if (i % 16 == 0)
						theWord = ReadInt16(data + j * pitch + ((i >> 3) & 0xfffe));

					D2(fprintf(stderr, "%s", ((theWord >> (15 - (i & 0xf))) & 1) ? "1" : " "));
					switch(logOp) {
					case 1:
						put_dtriplet(destAddress + offset, ((theWord >> (15 - (i & 0xf))) & 1) ? fgColor : bgColor);
						break;
					case 2:
						if ((theWord >> (15 - (i & 0xf))) & 1)
							put_dtriplet(destAddress + offset, fgColor);
						break;
					case 3:
						if ((theWord >> (15-(i&0xf))) & 1)
							put_dtriplet(destAddress + offset, ~get_dtriplet(destAddress + offset));
						break;
					case 4:
						if (!((theWord >> (15 - (i & 0xf))) & 1))
							put_dtriplet(destAddress + offset, fgColor);
						break;
					}
				}
				D2(bug("")); //newline
			}
			break;
		case 32:
			for(uint16 j = 0; j < h; j++) {
				D2(fprintf(stderr, "fVDI: bmp:"));

				uint16 theWord = ReadInt16(data + j * pitch + ((sx >> 3) & 0xfffe));
				for(uint16 i = sx; i < sx + w; i++) {
					uint32 offset = (dx + i - sx) * 4 + (dy + j) * destPitch;
					if (i % 16 == 0)
						theWord = ReadInt16(data + j * pitch + ((i >> 3) & 0xfffe));

					D2(fprintf(stderr, "%s", ((theWord >> (15 - (i & 0xf))) & 1) ? "1" : " "));
					switch(logOp) {
					case 1:
						WriteInt32(destAddress + offset, ((theWord >> (15 - (i & 0xf))) & 1) ? fgColor : bgColor);
						break;
					case 2:
						if ((theWord >> (15-(i&0xf))) & 1)
							WriteInt32(destAddress + offset, fgColor);
						break;
					case 3:
						if ((theWord >> (15 - (i & 0xf))) & 1)
							WriteInt32(destAddress + offset, ~ReadInt32(destAddress + offset));
						break;
					case 4:
						if (!((theWord >> (15 - (i & 0xf))) & 1))
							WriteInt32(destAddress + offset, fgColor);
						break;
					}
				}
				D2(bug("")); //newline
			}
			break;
		default:
			for(uint16 j = 0; j < h; j++) {
				D2(fprintf(stderr, "fVDI: bmp:"));

				uint16 theWord = ReadInt16(data + j * pitch + ((sx >> 3) & 0xfffe));
				for(uint16 i = sx; i < sx + w; i++) {
					uint32 wordIndex = ((dx + i - sx) >> 4) * destPlanes;
					uint32 offset = wordIndex * 2 + (dy + j) * destPitch;
					if (i % 16 == 0)
						theWord = ReadInt16(data + j * pitch + ((i >> 3) & 0xfffe));

					D2(fprintf(stderr, "%s", ((theWord >> (15 - (i & 0xf))) & 1) ? "1" : " "));
					switch(logOp) {
					case 1:
						for(uint16 d = 0; d < destPlanes; d++)
							WriteInt16(destAddress + offset + d * 2, (fgColor ? theWord : ~theWord));
						break;
					case 2:
						for(uint16 d = 0; d < destPlanes; d++)
							WriteInt16(destAddress + offset + d * 2, ReadInt16(destAddress + offset + d * 2) | (fgColor ? theWord : ~theWord));
						break;
					case 3:
						for(uint16 d = 0; d < destPlanes; d++)
							WriteInt16(destAddress + offset + d * 2, ~ReadInt16(destAddress + offset + d * 2));
						break;
					case 4:
						for(uint16 d = 0; d < destPlanes; d++)
							WriteInt16(destAddress + offset + d * 2, ReadInt16(destAddress + offset + d * 2) | (fgColor ? theWord : ~theWord));
						break;
					}
				}
				D2(bug("")); //newline
			}
			break;
		}

		return 1;
	}

	if (!hostScreen.renderBegin())
		return 1;

	D2(bug("fVDI: expandArea M->S"));
	for(uint16 j = 0; j < h; j++) {
		D2(fprintf(stderr, "fVDI: bmp:"));

		uint16 theWord = ReadInt16(data + j * pitch + ((sx >> 3) & 0xfffe));
		for(uint16 i = sx; i < sx + w; i++) {
			if (i % 16 == 0)
				theWord = ReadInt16(data + j * pitch + ((i >> 3) & 0xfffe));

			D2(fprintf(stderr, "%s", ((theWord >> (15 - (i & 0xf))) & 1) ? "1" : " "));
			switch(logOp) {
			case 1:
				hostScreen.putPixel(dx + i - sx, dy + j, ((theWord >> (15 - (i & 0xf))) & 1) ? fgColor : bgColor);
				break;
			case 2:
				if ((theWord >> (15 - (i & 0xf))) & 1)
					hostScreen.putPixel(dx + i - sx, dy + j, fgColor);
				break;
			case 3:
				if ((theWord >> (15 - (i & 0xf))) & 1)
					hostScreen.putPixel(dx + i - sx, dy + j, ~hostScreen.getPixel(dx + i - sx, dy + j));
				break;
			case 4:
				if (!((theWord >> (15 - (i & 0xf))) & 1))
					hostScreen.putPixel(dx + i - sx, dy + j, fgColor);
				break;
			}
		}
		D2(bug("")); //newline
	}

	hostScreen.renderEnd();
	hostScreen.update(dx, dy, w, h, true);

	return 1;
}


/**
 * Fill a coloured area using a monochrome pattern.
 *
 * c_fill_area(Virtual *vwk, long x, long y, long w, long h,
 *                           short *pattern, long colour, long mode, long interior_style)
 * fill_area
 * In:  a1  VDI struct
 *  d0  height and width to fill (high and low word)
 *  d1  x or table address
 *  d2  y or table length (high) and type (low)
 *  d3  pattern address
 *  d4  colour
 *
 * This function has two modes:
 * - single block to fill
 * - table based y/x1/x2 spans to fill (special mode 0 (low word of 'y'))
 *
 * As usual, only the first one is necessary, and a return with d0 = -1
 * signifies that a special mode should be broken down to the basic one.
 *
 * An immediate return with 0 gives a fallback (normally line based drawing
 * by the fVDI engine for solid fills, otherwise pixel by pixel).
 * A negative return will break down the special mode into separate calls,
 * with no more fallback possible.
 **/
int FVDIDriver::fillArea(memptr vwk, uint32 x_, uint32 y_, int32 w, int32 h,
                         memptr pattern_addr, uint32 colors, uint32 logOp, uint32 interior_style)
{
	uint32 fgColor = (int16)(colors & 0xffff);
	uint32 bgColor = (int16)(colors >> 16);

	uint16 pattern[16];
	for(int i = 0; i < 16; ++i)
		pattern[i] = ReadInt16(pattern_addr + i * 2);

	int16* table = 0;

	int x = x_;
	int y = y_;

	if ((long)vwk & 1) {
		if ((y_ & 0xffff) != 0)
			return -1;		// Don't know about this kind of table operation
		table = (int16*)x_;
		h = (y_ >> 16) & 0xffff;
		vwk -= 1;
	}

	D(bug("fVDI: %s %d %d,%d:%d,%d : %d,%d p:%x, (fgc:%d /%x/ : bgc:%d /%x/)", "fillArea",
	      logOp, x, y, w, h, x + w - 1, x + h - 1, *pattern,
	      fgColor, hostScreen.getPaletteColor(fgColor),
	      bgColor, hostScreen.getPaletteColor(bgColor)));

	if (hostScreen.getBpp() > 1) {
		fgColor = hostScreen.getPaletteColor(fgColor);
		bgColor = hostScreen.getPaletteColor(bgColor);
	}

	if (!hostScreen.renderBegin())
		return 1;

	int minx = 1000000;
	int miny = 1000000;
	int maxx = -1000000;
	int maxy = -1000000;

	/* Perform rectangle fill. */
	if (!table) {
		hostScreen.fillArea(x, y, w, h, pattern, fgColor, bgColor, logOp);
		minx = x;
		miny = y;
		maxx = x + w - 1;
		maxy = y + h - 1;
	} else {
		for(h = h - 1; h >= 0; h--) {
			y = (int16)ReadInt16((memptr)table++);
			x = (int16)ReadInt16((memptr)table++);
			w = (int16)ReadInt16((memptr)table++) - x + 1;
			hostScreen.fillArea(x, y, w, 1, pattern, fgColor, bgColor, logOp);
			if (x < minx)
				minx = x;
			if (y < miny)
				miny = y;
			if (x + w - 1 > maxx)
				maxx = x + w - 1;
			if (y > maxy)
				maxy = y;
		}
	}

	hostScreen.renderEnd();
	hostScreen.update(minx, miny, maxx - minx + 1, maxy - miny + 1, true);

	return 1;
}


/**
 * Blit an area
 *
 * c_blit_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
 *                        MFDB *dst, long dst_x, long dst_y,
 *                        long w, long h, long operation)
 * blit_area
 * In:  a1  VDI struct, destination MFDB, VDI struct, source MFDB
 *  d0  height and width to move (high and low word)
 *  d1-d2   source coordinates
 *  d3-d4   destination coordinates
 *  d7  logic operation
 *
 * Only one mode here.
 *
 * Note that a1 does not point to the VDI struct, but to a place in memory
 * where the VDI struct pointer can be found. Four bytes beyond that address
 * is a pointer to the destination MFDB, and then comes a VDI struct
 * pointer again (the same) and a pointer to the source MFDB.
 *
 * Since MFDBs are passed, the screen is not necessarily involved.
 *
 * A return with 0 gives a fallback (normally pixel by pixel drawing by the
 * fVDI engine).
 **/
#define applyBlitLogOperation(logicalOperation, destinationData, sourceData) \
	switch(logicalOperation) { \
	case 0: \
		destinationData = 0; \
		break; \
	case 1: \
		destinationData = sourceData & destinationData; \
		break; \
	case 2: \
		destinationData = sourceData & ~destinationData; \
		break; \
	case 3: \
		destinationData = sourceData; \
		break; \
	case 4: \
		destinationData = ~sourceData & destinationData; \
		break; \
	case 5: \
		destinationData = destinationData; \
		break; \
	case 6: \
		destinationData = sourceData ^ destinationData; \
		break; \
	case 7: \
		destinationData = sourceData | destinationData; \
		break; \
	case 8: \
		destinationData = ~(sourceData | destinationData); \
		break; \
	case 9: \
		destinationData = ~(sourceData ^ destinationData); \
		break; \
	case 10: \
		destinationData = ~destinationData; \
		break; \
	case 11: \
		destinationData = sourceData | ~destinationData; \
		break; \
	case 12: \
		destinationData = ~sourceData; \
		break; \
	case 13: \
		destinationData = ~sourceData | destinationData; \
		break; \
	case 14: \
		destinationData = ~(sourceData & destinationData); \
		break; \
	case 15: \
		destinationData = 0xffff; \
		break; \
	}


extern "C" {
static void chunkyToBitplane(uint8 *sdlPixelData, uint16 bpp, uint16 bitplaneWords[8])
{
	memset(bitplaneWords, 0, sizeof(bitplaneWords)); // clear the color values for the 8 pixels (word length)

	for (int l = 0; l < 16; l++) {
		uint8 data = sdlPixelData[l]; // note: this is about 2000 dryhstones speedup (the local variable)

		bitplaneWords[0] <<= 1; bitplaneWords[0] |= (data >> 0) & 1;
		bitplaneWords[1] <<= 1; bitplaneWords[1] |= (data >> 1) & 1;
		bitplaneWords[2] <<= 1; bitplaneWords[2] |= (data >> 2) & 1;
		bitplaneWords[3] <<= 1; bitplaneWords[3] |= (data >> 3) & 1;
		bitplaneWords[4] <<= 1; bitplaneWords[4] |= (data >> 4) & 1;
		bitplaneWords[5] <<= 1; bitplaneWords[5] |= (data >> 5) & 1;
		bitplaneWords[6] <<= 1; bitplaneWords[6] |= (data >> 6) & 1;
		bitplaneWords[7] <<= 1; bitplaneWords[7] |= (data >> 7) & 1;
	}
}
}

int FVDIDriver::blitArea(memptr vwk, memptr src, int32 sx, int32 sy, memptr dest, int32 dx, int32 dy,
                         int32 w, int32 h, uint32 logOp)
{
	D(bug("fVDI: %s %x %d,%d:%d,%d:%d,%d", "blitArea", logOp, sx, sy, dx, dy, w, h ));
	D(bug("fVDI: %s %x,%x : %x,%x", "blitArea - MFDB addresses", src, dest,
              (src) ? (ReadInt32(src)) : 0, (dest) ? (ReadInt32(dest)) : 0));

	bool toMemory = dest;
	if (src) { // fromMemory
		uint32 planes = ReadInt16(src + MFDB_NPLANES);			// MFDB *src->bitplanes
		uint32 pitch  = ReadInt16(src + MFDB_WDWIDTH) * planes * 2;	// MFDB *src->pitch
		memptr data   = ReadInt32(src) + sy * pitch;			// MFDB *src->address host OS address

		D(bug("fVDI: blitArea M->: address %x, pitch %d, planes %d", data, pitch, planes));

		if (toMemory) {
			// the destPlanes is always the same?
			planes = ReadInt16(dest + MFDB_NPLANES);		// MFDB *dest->bitplanes
			uint32 destPitch = ReadInt16(dest + MFDB_WDWIDTH) * planes * 2;	// MFDB *dest->pitch
			memptr destAddress = (memptr)ReadInt32(dest);

			D(bug("fVDI: blitArea M->M"));

			memptr srcData;
			memptr destData;

			switch(planes) {
			case 16:
				for(int32 j = 0; j < h; j++)
					for(int32 i = sx; i < sx + w; i++) {
						uint32 offset = (dx + i - sx) * 2 + (dy + j) * destPitch;
						srcData = ReadInt16(data + j * pitch + i * 2);
						destData = ReadInt16(destAddress + offset);
						applyBlitLogOperation(logOp, destData, srcData);
						WriteInt16(destAddress + offset, destData);
					}
				break;
			case 24:
				for(int32 j = 0; j < h; j++)
					for(int32 i = sx; i < sx + w; i++) {
						uint32 offset = (dx + i - sx) * 3 + (dy + j) * destPitch;
						srcData = get_dtriplet(data + j * pitch + i * 3);
						destData = get_dtriplet(destAddress + offset);
						applyBlitLogOperation(logOp, destData, srcData);
						put_dtriplet(destAddress + offset, destData);
					}
				break;
			case 32:
				for(int32 j = 0; j < h; j++)
					for(int32 i = sx; i < sx + w; i++) {
						uint32 offset = (dx + i - sx) * 4 + (dy + j) * destPitch;
						srcData = ReadInt32(data + j * pitch + i * 4);
						destData = ReadInt32(destAddress + offset);
						applyBlitLogOperation(logOp, destData, srcData);
						WriteInt32(destAddress + offset, destData);
					}
				break;

			default:
				if (planes < 16) {
					D(bug("fVDI: blitArea M->M: NOT TESTED bitplaneToCunky conversion"));
				}
			}
			return 1;
		}


		D(bug("fVDI: blitArea M->S"));

		if (!hostScreen.renderBegin())
			return 1;

		D(bug("fVDI: M->S: address %x, pitch %d, planes %d", data, pitch, planes));

		memptr srcData;
		memptr destData;

		switch(planes) {
		case 16:
			if (logOp != 3) {
				for(int32 j = 0; j < h; j++)
					for(int32 i = sx; i < sx + w; i++) {
						srcData = ReadInt16(data + j * pitch + i * 2);
						//uint16 destData = ReadInt16(videoRam + (dx + i - sx) * 2 + (dy + j) * screenPitch); // shadow?
						destData = hostScreen.getPixel(dx + i - sx, dy + j);
						applyBlitLogOperation(logOp, destData, srcData);
						//WriteInt16(videoRam + (dx + i - sx) * 2 + (dy + j) * screenPitch, destData); // shadow?
						hostScreen.putPixel(dx + i - sx, dy + j, destData);
					}
			} else {
				uint16* daddr_base = (uint16*)((long)hostScreen.getVideoramAddress() +
				                          dy * hostScreen.getPitch() + dx * 2);
				memptr saddr_base = data + sx * 2;
				for(int32 j = 0; j < h; j++) {
					uint16* daddr = daddr_base;
					memptr saddr = saddr_base;
					daddr_base += hostScreen.getPitch() / 2;
					saddr_base += pitch;
					for(int32 i = 0; i < w; i++) {
						destData = ReadInt16(saddr);
						saddr += 2;
						*daddr++ = destData;
					}
				}
			}
			break;
		case 24:
			for(int32 j = 0; j < h; j++)
				for(int32 i = sx; i < sx + w; i++) {
					srcData = get_dtriplet(data + j * pitch + i * 3);
					destData = hostScreen.getPixel(dx + i - sx, dy + j);
					applyBlitLogOperation(logOp, destData, srcData);
					hostScreen.putPixel(dx + i - sx, dy + j, destData);
				}
			break;
		case 32:
			for(int32 j = 0; j < h; j++)
				for(int32 i = sx; i < sx + w; i++) {
					srcData = ReadInt32(data + j * pitch + i * 4);
					destData = hostScreen.getPixel(dx + i - sx, dy + j);
					applyBlitLogOperation(logOp, destData, srcData);
					hostScreen.putPixel(dx + i - sx, dy + j, destData);
				}
			break;

		default: // bitplane modes...
			if (planes < 16) {
				uint8 color[16];

				D(bug("fVDI: blitArea M->S: bitplaneToCunky conversion"));
				uint16 *dataHost = (uint16*)Atari2HostAddr(data); // FIXME: Hack! Should use the get_X() methods

				for(int32 j = 0; j < h; j++) {
					uint32 wordIndex = (j * pitch >> 1) + (sx >> 4) * planes;
					hostScreen.bitplaneToChunky(&dataHost[wordIndex], planes, color);

					for(int32 i = sx; i < sx + w; i++) {
						uint8 bitNo = i & 0xf;
						if (bitNo == 0) {
							uint32 wordIndex = (j * pitch >> 1) + (i >> 4) * planes;
							hostScreen.bitplaneToChunky(&dataHost[wordIndex], planes, color);
						}
						hostScreen.putPixel(dx + i - sx, dy + j, color[bitNo]);
					}
				}
			}
		}

		hostScreen.renderEnd();
		hostScreen.update(dx, dy, w, h, true);

		return 1;
	}

	if (toMemory) {
		D(bug("fVDI: blitArea S->M"));

		if (!hostScreen.renderBegin())
			return 1;

		//uint32 planes = screenPlanes;
		//uint32 pitch  = screenPitch;
		//memptr data   = videoRam + sy*pitch;

		uint32 planes = ReadInt16(dest + MFDB_NPLANES);			// MFDB *dest->bitplanes
		uint32 destPitch = ReadInt16(dest + MFDB_WDWIDTH) * planes * 2;	// MFDB *dest->pitch
		memptr destAddress = ReadInt32(dest);

		D(bug("fVDI: S->M: address %x, pitch %d, planes %d", destAddress, destPitch, planes));

		memptr srcData;
		memptr destData;

		switch(planes) {
		case 16:
			for(int32 j = 0; j < h; j++)
				for(int32 i = sx; i < sx + w; i++) {
					uint32 offset = (dx + i - sx) * 2 + (dy + j) * destPitch;
					//uint16 srcData = ReadInt16(data + i * 2 + j * pitch ); // from the shadow?
					srcData = hostScreen.getPixel(i, sy + j);
					destData = ReadInt16(destAddress + offset);
					applyBlitLogOperation(logOp, destData, srcData);
					WriteInt16(destAddress + offset, destData);
				}
			break;
		case 24:
			for(int32 j = 0; j < h; j++)
				for(int32 i = sx; i < sx + w; i++) {
					uint32 offset = (dx + i - sx) * 3 + (dy + j) * destPitch;
					srcData = hostScreen.getPixel( i, sy + j );
					destData = get_dtriplet(destAddress + offset);
					applyBlitLogOperation(logOp, destData, srcData);
					put_dtriplet(destAddress + offset, destData);
				}
			break;
		case 32:
			for(int32 j = 0; j < h; j++)
				for(int32 i = sx; i < sx + w; i++) {
					uint32 offset = (dx + i - sx) * 4 + (dy + j) * destPitch;
					srcData = hostScreen.getPixel(i, sy + j);
					destData = ReadInt32(destAddress + offset);
					applyBlitLogOperation(logOp, destData, srcData);
					WriteInt32(destAddress + offset, destData);
				}
			break;

		default:
			if (planes < 16) {
				D(bug("fVDI: blitArea S->M: bitplane conversion"));

				uint16 bitplanePixels[8];
				uint32 pitch = hostScreen.getPitch();
				uint8* dataHost = (uint8*)hostScreen.getVideoramAddress() + sy * pitch;

				for(int32 j = 0; j < h; j++) {
					uint32 pixelPosition = j * pitch + sx & ~0xf; // div 16
					chunkyToBitplane(dataHost + pixelPosition, planes, bitplanePixels);
					for(uint32 d = 0; d < planes; d++)
						WriteInt16(destAddress + (((dx >> 4) * planes) + d) * 2 + (dy + j) * destPitch, bitplanePixels[d]);

					for(int32 i = sx; i < sx + w; i++) {
						uint8 bitNo = i & 0xf;
						if (bitNo == 0) {
							uint32 wordIndex = ((dx + i - sx) >> 4) * planes;
							uint32 pixelPosition = j * pitch + i & ~0xf; // div 16
							chunkyToBitplane(dataHost + pixelPosition, planes, bitplanePixels);
							for(uint32 d = 0; d < planes; d++)
								WriteInt16(destAddress + (wordIndex + d) * 2 + (dy + j) * destPitch, bitplanePixels[d]);
						}
					}
				}
			}
		}

		hostScreen.renderEnd();
		return 1;
	}

	D(bug("fVDI: %s ", "blitArea - S->S!"));

	// if (!hostScreen.renderBegin()) // the surface must _not_ be locked for blitArea (SDL_BlitSurface)
	//      return 1;

	// FIXME: There are no logical operation implementation ATM
	// for S->S blits... -> SDL does the whole thing at once
	if (logOp == 3)
		hostScreen.blitArea(sx, sy, dx, dy, w, h);
	else
		D(bug("fVDI: %s %d", "blitArea - S->S! mode NOT IMPLEMENTED!!!", logOp));

	hostScreen.update(sx, sy, w, h, true);
	hostScreen.update(dx, dy, w, h, true);

	return 1;
}


/**
 * Draw a coloured line between two points
 *
 * c_draw_line(Virtual *vwk, long x1, long y1, long x2, long y2,
 *                          long pattern, long colour)
 * draw_line
 * In:  a1  VDI struct
 *  d0  logic operation
 *  d1  x1 or table address
 *  d2  y1 or table length (high) and type (low)
 *  d3  x2 or move point count
 *  d4  y2 or move index address
 *  d5  pattern
 *  d6  colour
 *
 * This function has three modes:
 * - single line
 * - table based coordinate pairs (special mode 0 (low word of 'y1'))
 * - table based coordinate pairs+moves (special mode 1)
 *
 * As usual, only the first one is necessary, and a return with d0 = -1
 * signifies that a special mode should be broken down to the basic one.
 *
 * An immediate return with 0 gives a fallback (normally pixel by pixel
 * drawing by the fVDI engine).
 * A negative return will break down the special modes into separate calls,
 * with no more fallback possible.
 **/


/* --------- Clipping routines for box/line */

/* Clipping based heavily on code from                       */
/* http://www.ncsa.uiuc.edu/Vis/Graphics/src/clipCohSuth.c   */

#define CLIP_LEFT_EDGE   0x1
#define CLIP_RIGHT_EDGE  0x2
#define CLIP_BOTTOM_EDGE 0x4
#define CLIP_TOP_EDGE    0x8
#define CLIP_INSIDE(a)   (!a)
#define CLIP_REJECT(a,b) (a&b)
#define CLIP_ACCEPT(a,b) (!(a|b))


static inline int clipEncode (int x, int y, int left, int top, int right, int bottom)
{
	int code = 0;
	if (x < left) {
		code |= CLIP_LEFT_EDGE;
	} else if (x > right) {
		code |= CLIP_RIGHT_EDGE;
	}
	if (y < top) {
		code |= CLIP_TOP_EDGE;
	} else if (y > bottom) {
		code |= CLIP_BOTTOM_EDGE;
	}
	return code;
}


static inline bool clipLine(int& x1, int& y1, int& x2, int& y2, int cliprect[])
{
#if OLD_CODE // check what is bad!
	if (!cliprect) {
		return true; // Clipping is off
	// Get clipping boundary
 	int left   = cliprect[0];
 	int top    = cliprect[1];
 	int right  = cliprect[2];
 	int bottom = cliprect[3];
#else
	// Get clipping boundary
	int left, top, right, bottom;

	if (!cliprect) {
		left   = 0;
		top    = 0;
		right  = hostScreen.getWidth() -1;
		bottom = hostScreen.getHeight()-1;
	} else {
		left   = cliprect[0];
		top    = cliprect[1];
		right  = cliprect[2];
		bottom = cliprect[3];
	}
#endif

	bool draw = false;
	while (1) {
		int code1 = clipEncode(x1, y1, left, top, right, bottom);
		int code2 = clipEncode(x2, y2, left, top, right, bottom);
		if (CLIP_ACCEPT(code1, code2)) {
			draw = true;
			break;
		} else if (CLIP_REJECT(code1, code2)) {
			break;
		} else {
			if (CLIP_INSIDE(code1)) {
				int swaptmp = x2; x2 = x1; x1 = swaptmp;
				swaptmp = y2; y2 = y1; y1 = swaptmp;
				swaptmp = code2; code2 = code1; code1 = swaptmp;
			}
			float m = 1.0f;
			if (x2 != x1) {
				m = (y2 - y1) / (float)(x2 - x1);
			}
			if (code1 & CLIP_LEFT_EDGE) {
				y1 += (int)((left - x1) * m);
				x1 = left;
			} else if (code1 & CLIP_RIGHT_EDGE) {
				y1 += (int)((right - x1) * m);
				x1 = right;
			} else if (code1 & CLIP_BOTTOM_EDGE) {
				if (x2 != x1) {
					x1 += (int)((bottom - y1) / m);
				}
				y1 = bottom;
			} else if (code1 & CLIP_TOP_EDGE) {
				if (x2 != x1) {
					x1 += (int)((top - y1) / m);
				}
				y1 = top;
			}
		}
	}

	D2(bug("fVDI: %s %d,%d:%d,%d", "clipLineEND", x1, y1, x2, y2));

	return draw;
}


// Don't forget rotation of pattern!
int FVDIDriver::drawSingleLine(int x1, int y1, int x2, int y2, uint16 pattern,
                               uint32 fgColor, uint32 bgColor, int logOp, bool last_pixel,
                               int cliprect[], int minmax[])
{
	if (clipLine(x1, y1, x2, y2, cliprect)) {	// Do not draw the line when it is out
		D(bug("fVDI: %s %d,%d:%d,%d", "drawSingleLine", x1, y1, x2, y2));
		hostScreen.drawLine(x1, y1, x2, y2, pattern, fgColor, bgColor, logOp, last_pixel);
		if (x1 < x2) {
			if (x1 < minmax[0])
				minmax[0] = x1;
			if (x2 > minmax[2])
				minmax[2] = x2;
		} else {
			if (x2 < minmax[0])
				minmax[0] = x2;
			if (x1 > minmax[2])
				minmax[2] = x1;
		}
		if (y1 < y2) {
			if (y1 < minmax[1])
				minmax[1] = y1;
			if (y2 > minmax[3])
				minmax[3] = y2;
		} else {
			if (y2 < minmax[1])
				minmax[1] = y2;
			if (y1 > minmax[3])
				minmax[3] = y1;
		}
	}
	return 1;
}


// Don't forget rotation of pattern!
int FVDIDriver::drawTableLine(int16 table[], int length, uint16 pattern,
                              uint32 fgColor, uint32 bgColor, int logOp,
                              int cliprect[], int minmax[])
{
	int x1 = (int16)ReadInt16((memptr)table++);
	int y1 = (int16)ReadInt16((memptr)table++);
	for(--length; length > 0; length--) {
		int x2 = (int16)ReadInt16((memptr)table++);
		int y2 = (int16)ReadInt16((memptr)table++);

		drawSingleLine(x1, y1, x2, y2, pattern, fgColor, bgColor,
		               logOp, length == 1, cliprect, minmax);
		x1 = x2;
		y1 = y2;
	}

	return 1;
}


// Don't forget rotation of pattern!
int FVDIDriver::drawMoveLine(int16 table[], int length, uint16 index[], int moves, uint16 pattern,
                             uint32 fgColor, uint32 bgColor, int logOp,
                             int cliprect[], int minmax[])
{
	int x1 = (int16)ReadInt16((memptr)table++);
	int y1 = (int16)ReadInt16((memptr)table++);
	moves--;
	if ((int16)ReadInt16((memptr)&index[moves]) == -4)
		moves--;
	if ((int16)ReadInt16((memptr)&index[moves]) == -2)
		moves--;
	int movepnt = -1;
	if (moves >= 0)
		movepnt = ((int16)ReadInt16((memptr)&index[moves]) + 4) / 2;
	for(int n = 1; n < length; n++) {
		int x2 = (int16)ReadInt16((memptr)table++);
		int y2 = (int16)ReadInt16((memptr)table++);
		if (n == movepnt) {
			if (--moves >= 0)
				movepnt = ((int16)ReadInt16((memptr)&index[moves]) + 4) / 2;
			else
				movepnt = -1;		/* Never again equal to n */
			x1 = x2;
			y1 = y2;
			continue;
		}

		drawSingleLine(x1, y1, x2, y2, pattern, fgColor, bgColor,
		               logOp, length == 1, cliprect, minmax);

		x1 = x2;
		y1 = y2;
	}

	return 1;
}


int FVDIDriver::drawLine(memptr vwk, uint32 x1_, uint32 y1_, uint32 x2_, uint32 y2_,
                         uint32 pattern, uint32 colors, uint32 logOp, memptr clip)
{
	uint32 fgColor = (int16)(colors & 0xffff);
	uint32 bgColor = (int16)(colors >> 16);
	if (hostScreen.getBpp() > 1) {
		fgColor = hostScreen.getPaletteColor(fgColor);
		bgColor = hostScreen.getPaletteColor(bgColor);
	}

	int16* table = 0;
	uint16* index = 0;
	int length = 0;
	int moves = 0;

	int x1 = (int16)x1_;
	int y1 = (int16)y1_;
	int x2 = (int16)x2_;
	int y2 = (int16)y2_;

	if (vwk & 1) {
		if ((unsigned)(y1 & 0xffff) > 1)
			return -1;		/* Don't know about this kind of table operation */
		table = (int16*)x1_;
		length = (y1_ >> 16) & 0xffff;
		if ((y1_ & 0xffff) == 1) {
			index = (uint16*)y2_;
			moves = x2_ & 0xffff;
		}
		vwk -= 1;
		x1 = (int16)ReadInt16((memptr)&table[0]);
		y1 = (int16)ReadInt16((memptr)&table[1]);
		x2 = (int16)ReadInt16((memptr)&table[2]);
		y2 = (int16)ReadInt16((memptr)&table[3]);
	}

	int cliparray[4];
	int* cliprect = 0;
	if (clip) {				// Clipping is not off
		cliprect = cliparray;
		cliprect[0] = (int16)ReadInt32(clip);
		cliprect[1] = (int16)ReadInt32(clip + 4);
		cliprect[2] = (int16)ReadInt32(clip + 8);
		cliprect[3] = (int16)ReadInt32(clip + 12);
		D2(bug("fVDI: %s %d,%d:%d,%d", "clipLineTO", cliprect[0], cliprect[1],
		       cliprect[2], cliprect[3]));
	}

	int minmax[4] = {1000000, 1000000, -1000000, -1000000};

#if TEST_STRAIGHT	// Not yet working
	int eq_coord = (x1 == x2) + 2 * (y1 == y2);
#endif

	if (table) {
		if (moves)
			drawMoveLine(table, length, index, moves, pattern, fgColor, bgColor,
			             logOp, cliprect, minmax);
		else {
#if TEST_STRAIGHT	// Not yet working
			if (eq_coord && ((pattern & 0xffff) == 0xffff) && (logOp < 3)) {
				table += 4;
				for(--length; length > 0; length--) {
					if (eq_coord & 1) {
						if (y1 < y2)
							vertical(x1, y1, 1, y2 - y1 + 1);
						else
							vertical(x2, y2, 1, y1 - y2 + 1);
					} else if (eq_coord & 2) {
						if (x1 < x2)
							horizontal(x1, y1, x2 - x1 + 1, 1);
						else
							horizontal(x2, y2, x1 - x2 + 1, 1);
					} else {
						length++;
						table -= 4;
						break;
					}
					x1 = x2;
					y1 = y2;
					x2 = (int16)ReadInt16((memptr)table++);
					y2 = (int16)ReadInt16((memptr)table++);
					eq_coord = (x1 == x2) + 2 * (y1 == y2);
				}
			}
#endif
			switch (length) {
			case 0:
				break;
			case 1:
				drawSingleLine(x1, y1, x2, y2, pattern, fgColor, bgColor,
				               logOp, true, cliprect, minmax);
				break;
			default:
				drawTableLine(table, length, pattern, fgColor, bgColor,
				              logOp, cliprect, minmax);
 				break;
			}
 		}
#if TEST_STRAIGHT	// Not yet working
	} else if (eq_coord && ((pattern & 0xffff) == 0xffff)) {
		if (eq_coord & 1) {
			if (y1 < y2)
				vertical(x1, y1, 1, y2 - y1 + 1);
			else
				vertical(x2, y2, 1, y1 - y2 + 1);
		} else {
			if (x1 < x2)
				horizontal(x1, y1, x2 - x1 + 1, 1);
			else
				horizontal(x2, y2, x1 - x2 + 1, 1);
		}
#endif
	} else
		drawSingleLine(x1, y1, x2, y2, pattern, fgColor, bgColor,
		               logOp, true, cliprect, minmax);

	if (minmax[0] != 1000000) {
		D(bug("fVDI: %s %d,%d:%d,%d", "drawLineUp",
		       minmax[0], minmax[1], minmax[2], minmax[3]));
		hostScreen.update(minmax[0], minmax[1],
		                  minmax[2] - minmax[0] + 1, minmax[3] - minmax[1] + 1, true);
	} else {
		D2(bug("fVDI: drawLineUp nothing to redraw"));
	}

	return 1;
}


// This is supposed to be a fast 16x16/16 with 32 bit intermediate result
#define SMUL_DIV(x,y,z)	((short)(((x)*(long)(y))/(z)))
// Some other possible variants are
//#define SMUL_DIV(x,y,z)	((long)(y - y1) * (x2 - x1) / (y2 - y1))
//#define SMUL_DIV(x,y,z)	((short)(((short)(x)*(long)((short)(y)))/(short)(z)))

int FVDIDriver::fillPoly(memptr vwk, memptr points_addr, int n, memptr index_addr, int moves,
                         memptr pattern_addr, int32 colors, uint32 logOp, uint32 interior_style, memptr clip)
{
	if (vwk & 1)
		return -1;      // Don't know about any special fills

	uint32 fgColor = (int16)(colors & 0xffff);
	uint32 bgColor = (int16)(colors >> 16);

	if (hostScreen.getBpp() > 1) {
		fgColor = hostScreen.getPaletteColor(fgColor);
		bgColor = hostScreen.getPaletteColor(bgColor);
	}

	// Allocate arrays for data
	if (!AllocPoints(n) || !AllocIndices(moves) || !AllocCrossings(200))
		return -1;

	uint16 pattern[16];
	for(int i = 0; i < 16; ++i)
		pattern[i] = ReadInt16(pattern_addr + i * 2);

	int cliparray[4];
	int* cliprect = 0;
	if (clip) {	// Clipping is not off
		cliprect = cliparray;
		cliprect[0] = (int16)ReadInt32(clip);
		cliprect[1] = (int16)ReadInt32(clip + 4);
		cliprect[2] = (int16)ReadInt32(clip + 8);
		cliprect[3] = (int16)ReadInt32(clip + 12);
		D2(bug("fVDI: %s %d,%d:%d,%d", "clipLineTO", cliprect[0], cliprect[1],
		       cliprect[2], cliprect[3]));
	}

	Points p(alloc_point);
	int16* index = alloc_index;
	int16* crossing = alloc_crossing;

	for(int i = 0; i < n; ++i) {
		p[i][0] = (int16)ReadInt16(points_addr + i * 4);
		p[i][1] = (int16)ReadInt16(points_addr + i * 4 + 2);
	}
	bool indices = moves;
	for(int i = 0; i < moves; ++i)
		index[i] = (int16)ReadInt16(index_addr + i * 2);


	if (!n)
		return 1;

	if (!hostScreen.renderBegin())
		return 1;

	if (!indices) {
		if ((p[0][0] == p[n - 1][0]) && (p[0][1] == p[n - 1][1]))
			n--;
	} else {
		moves--;
		if (index[moves] == -4)
			moves--;
		if (index[moves] == -2)
			moves--;
	}

	int miny = p[0][1];
	int maxy = miny;
	for(int i = 1; i < n; ++i) {
		int16 y = p[i][1];
		if (y < miny) {
			miny = y;
		}
		if (y > maxy) {
			maxy = y;
		}
	}
	if (cliprect) {
		if (miny < cliprect[1])
			miny = cliprect[1];
		if (maxy > cliprect[3])
			maxy = cliprect[3];
	}

	int minx = 1000000;
	int maxx = -1000000;

	for(int16 y = miny; y <= maxy; ++y) {
		int ints = 0;
		int16 x1 = 0;	// Make the compiler happy with some initializations
		int16 y1 = 0;
		int16 x2 = 0;
		int16 y2 = 0;
		int move_n = 0;
		int movepnt = 0;
		if (indices) {
			move_n = moves;
			movepnt = (index[move_n] + 4) / 2;
			x2 = p[0][0];
			y2 = p[0][1];
		} else {
			x1 = p[n - 1][0];
			y1 = p[n - 1][1];
		}

		for(int i = indices; i < n; ++i) {
			if (AllocCrossings(ints + 1))
				crossing = alloc_crossing;
			else
				break;		// At least something will get drawn

			if (indices) {
				x1 = x2;
				y1 = y2;
			}
			x2 = p[i][0];
			y2 = p[i][1];
			if (indices) {
				if (i == movepnt) {
					if (--move_n >= 0)
						movepnt = (index[move_n] + 4) / 2;
					else
						movepnt = -1;		// Never again equal to n
					continue;
				}
			}

			if (y1 < y2) {
				if ((y >= y1) && (y < y2)) {
					crossing[ints++] = SMUL_DIV((y - y1), (x2 - x1), (y2 - y1)) + x1;
				}
			} else if (y1 > y2) {
				if ((y >= y2) && (y < y1)) {
					crossing[ints++] = SMUL_DIV((y - y2), (x1 - x2), (y1 - y2)) + x2;
				}
			}
			if (!indices) {
				x1 = x2;
				y1 = y2;
			}
		}

		for(int i = 0; i < ints - 1; ++i) {
			for(int j = i + 1; j < ints; ++j) {
				if (crossing[i] > crossing[j]) {
					int16 tmp = crossing[i];
					crossing[i] = crossing[j];
					crossing[j] = tmp;
				}
			}
		}

		x1 = cliprect[0];
		x2 = cliprect[2];
		for(int i = 0; i < ints - 1; i += 2) {
			y1 = crossing[i];	// Really x-values, but...
			y2 = crossing[i + 1];
			if (y1 < x1)
				y1 = x1;
			if (y2 > x2)
				y2 = x2;
			if (y1 <= y2) {
				hostScreen.fillArea(y1, y, y2 - y1 + 1, 1, pattern,
				                    fgColor, bgColor, logOp);
				if (y1 < minx)
					minx = y1;
				if (y2 > maxx)
					maxx = y2;
			}
		}
	}

	hostScreen.renderEnd();
	if (minx != 1000000)
		hostScreen.update(minx, miny, maxx - minx + 1, maxy - miny + 1, true);

	return 1;
}


/*
 * $Log$
 * Revision 1.40  2002/09/15 15:17:15  joy
 * CPU to separate thread
 *
 * Revision 1.39  2002/08/03 12:36:42  johan
 * Updated to work with new API (dependencies on internal fVDI structures
 * have been removed and all parameters are passed on the stack).
 * Some internal fixes and cleanups.
 *
 * Revision 1.38  2002/06/24 17:08:48  standa
 * The pointer arithmetics fixed. The memptr usage introduced in my code.
 *
 * Revision 1.37  2002/04/22 18:30:50  milan
 * header files reform
 *
 * Revision 1.36  2002/03/27 20:01:47  standa
 * fVDI work also without the --fixedvideoram option. There is a new
 * getVideoramAddress native function. This might be reqested to be removed
 * in the future due to its missuse. The solution would be to make a
 * very special fVDI structure aware function that would set the address
 * to the required places.
 *
 * Revision 1.35  2002/02/19 20:04:05  milan
 * src/ <-> CPU interaction cleaned
 * memory access cleaned
 *
 * Revision 1.1.1.1  2002/02/10 23:32:50  jurikm
 * Unofficial ARAnyM
 *
 * Revision 1.34  2002/01/17 14:59:19  milan
 * cleaning in HW <-> memory communication
 * support for JIT CPU
 *
 * Revision 1.33  2002/01/13 23:08:49  standa
 * The fVDI driver expandArea 24 and 32bit patch. 1, 2 and 4bit depth driver
 * configuration available.
 *
 * Revision 1.32  2002/01/09 19:37:33  standa
 * The fVDI driver patched to not to pollute the HostScreen class getPaletteColor().
 *
 * Revision 1.31  2002/01/08 22:40:00  standa
 * The palette fix and a little 8bit driver update.
 *
 * Revision 1.30  2002/01/08 21:20:57  standa
 * fVDI driver palette store on res change implemented.
 *
 * Revision 1.29  2001/12/29 17:03:23  joy
 * Johan: new(nothrow) => new(std:nothrow)
 *
 * Revision 1.28  2001/12/17 08:33:00  standa
 * Thread synchronization added. The check_event and fvdidriver actions are
 * synchronized each to other.
 *
 * Revision 1.27  2001/12/12 02:17:36  standa
 * Some line clipping bug fixed (the clip is on every time now -> should fix).
 *
 * Revision 1.26  2001/12/11 21:03:57  standa
 * Johan's patch caused DEBUG directive to fail e.g. in main.cpp.
 * The inline functions were put into the .cpp file.
 *
 * Revision 1.25  2001/11/29 23:51:56  standa
 * Johan Klockars <rand@cd.chalmers.se> fVDI driver changes.
 *
 * Revision 1.24  2001/11/26 16:07:57  standa
 * Olivier Landemarre found bug fixed.
 *
 * Revision 1.23  2001/11/19 01:39:10  standa
 * The first bitplane mode version. The blit and expand M->M needs to be
 * implemented. There is some pixel shift in blitting that I can't find.
 *
 * Revision 1.22  2001/10/31 23:17:38  standa
 * fVDI driver update The 16,24 and 32bit mode should work.
 *
 * Revision 1.21  2001/10/30 22:59:34  standa
 * The resolution change is now possible through the fVDI driver.
 *
 * Revision 1.20  2001/10/29 23:15:26  standa
 * The blitArea method rewitten to use macros. More readable code.
 *
 * Revision 1.19  2001/10/24 17:55:01  standa
 * The fVDI driver fixes. Finishing the functionality tuning.
 *
 * Revision 1.18  2001/10/23 21:28:49  standa
 * Several changes, fixes and clean up. Shouldn't crash on high resolutions.
 * hostscreen/gfx... methods have fixed the loop upper boundary. The interface
 * types have changed quite havily.
 *
 * Revision 1.17  2001/10/19 11:58:46  standa
 * The line clipping added (has bug, when neither one point is within the rect).
 * expandArea not handles the spacial mode (fallbacks it).
 * working with fVDI beta.
 *
 * Revision 1.16  2001/10/16 09:07:36  standa
 * The fVDI expandArea M->M implemented. The 16bit driver should work now.
 *
 * Revision 1.15  2001/10/14 16:11:54  standa
 * Syntax fix. STanda's calls to videl.renderNoFlag commented out.
 *
 * Revision 1.14  2001/10/12 08:25:38  standa
 * The fVDI blitting fixed and extended. Now only the expandToMemory and
 * blit mem2mem is to be done.
 *
 * Revision 1.13  2001/10/03 06:37:41  standa
 * General cleanup. Some constants added. Better "to screen" operation
 * recognition (the videoram address is checked too - instead of only the
 * MFDB == NULL || MFDB->address == NULL)
 *
 * Revision 1.12  2001/10/01 22:22:41  standa
 * bitplaneToChunky conversion moved into HostScreen (inline - should be no performance penalty).
 * fvdidrv/blitArea form memory works in TC.
 *
 * Revision 1.11  2001/09/30 23:09:23  standa
 * The line logical operation added.
 * The first version of blitArea (screen to screen only).
 *
 * Revision 1.10  2001/09/24 23:16:28  standa
 * Another minor changes. some logical operation now works.
 * fvdidrv/fillArea and fvdidrv/expandArea got the first logOp handling.
 *
 * Revision 1.9  2001/09/21 13:57:32  standa
 * D2(x) used - see include/debug.h
 *
 * Revision 1.8  2001/09/21 06:53:26  standa
 * The blitting is now said to be working, although it is not implemented.
 * expand to memory (not to SDL surface) is not either processed or dispalyed.
 *
 * Revision 1.7  2001/09/20 18:12:09  standa
 * Off by one bug fixed in fillArea.
 * Separate functions for transparent and opaque background.
 * gfxPrimitives methods moved to the HostScreen
 *
 * Revision 1.6  2001/09/19 23:03:46  standa
 * The fVDI driver update. Basic expandArea was added to display texts.
 * Still heavy buggy code!
 *
 * Revision 1.5  2001/08/30 14:04:59  standa
 * The fVDI driver. mouse_draw implemented. Partial pattern fill support.
 * Still buggy.
 *
 * Revision 1.4  2001/08/28 23:26:09  standa
 * The fVDI driver update.
 * VIDEL got the doRender flag with setter setRendering().
 *       The host_colors_uptodate variable name was changed to hostColorsSync.
 * HostScreen got the doUpdate flag cleared upon initialization if the HWSURFACE
 *       was created.
 * fVDIDriver got first version of drawLine and fillArea (thanks to SDL_gfxPrimitives).
 *
 * Revision 1.3  2001/08/09 12:35:43  standa
 * Forced commit to sync the CVS. ChangeLog should contain all details.
 *
 * Revision 1.2  2001/07/24 06:41:25  joy
 * updateScreen removed.
 * Videl reference should be removed as well.
 *
 * Revision 1.1  2001/06/18 15:48:42  standa
 * fVDI driver object.
 *
 *
 */
