/*
 *	IKBD 6301 emulation
 *
 *	Joy 2001
 *	Patrice Mandin
 */

#include "sysdeps.h"
#include "hardware.h"
#include "cpu_emulation.h"
#include "memory.h"
#include "ikbd.h"

#define DEBUG 0
#include "debug.h"

/*--- Defines ---*/

#define DEFAULT_INBUFFERLEN (1<<6)
#define DEFAULT_OUTBUFFERLEN (1<<4)

#define JOYSTICK_THRESHOLD	16384

/*--- Constructor/destructor of the IKBD class ---*/

IKBD::IKBD() : ACIA(HW_IKBD)
{
	rwLock = SDL_CreateMutex();

	D(bug("ikbd: interface created at 0x%06x",HW_IKBD));

	inbufferlen = DEFAULT_INBUFFERLEN;
	outbufferlen = DEFAULT_OUTBUFFERLEN;

	inbuffer = new uae_u8[inbufferlen];
	outbuffer = new uae_u8[outbufferlen];

	reset();
};

IKBD::IKBD(int inlen, int outlen) : ACIA(HW_IKBD)
{
	rwLock = SDL_CreateMutex();

	D(bug("ikbd: interface created at 0x%06x",HW_IKBD));

	inbufferlen = inlen;
	outbufferlen = outlen;

	inbuffer = new uae_u8[inbufferlen];
	outbuffer = new uae_u8[outbufferlen];

	reset();
};

IKBD::~IKBD()
{
	SDL_DestroyMutex(rwLock);

	delete outbuffer;
	outbuffer = NULL;
	delete inbuffer;
	inbuffer = NULL;

	D(bug("ikbd: interface destroyed at 0x%06x",baseaddr));
}

/*--- IKBD i/o access functions ---*/

void IKBD::reset()
{
	inread = inwrite = 0;
	outwrite = 0;
	intype = IKBD_PACKET_UNKNOWN;

	/* Default: mouse on port 0 enabled */
	mouse_enabled = SDL_TRUE;
	mouserel_enabled = SDL_TRUE;
	mousex = mousey = mouseb = 0;
	
	/* Default: joystick on port 0 disabled */
	joy_enabled[0] = SDL_FALSE;
	joy_state[0] = 0;
	
	/* Default: joystick on port 1 enabled */
	joy_enabled[1] = SDL_TRUE;
	joy_state[1] = 0;

	D(bug("ikbd: reset"));
}

uae_u8 IKBD::ReadStatus()
{
	D(bug("ikbd: ReadStatus()=0x%02x",sr));
	return sr;
}

void IKBD::WriteControl(uae_u8 value)
{
	SDL_LockMutex(rwLock);
	cr = value;
	SDL_UnlockMutex(rwLock);

	D(bug("ikbd: WriteControl(0x%02x)",cr));

#if DEBUG
	PrintControlRegister("ikbd",cr);
#endif
}

uae_u8 IKBD::ReadData()
{
	SDL_LockMutex(rwLock);

	if (inread != inwrite) {

		rxdr = inbuffer[inread++];
		D(bug("ikbd: ReadData()=0x%02x at position %d",rxdr, inread-1));

		inread &= (inbufferlen-1);

		if (inread == inwrite) {
			/* Queue empty */

			/* Update MFP GPIP */
			uae_u8 x = ReadAtariInt8(0xfffa01);
			x |= 0x10;
			WriteAtariInt8(0xfffa01, x);

			sr &= ~((1<<ACIA_SR_INTERRUPT)|(1<<ACIA_SR_RXFULL));

		} else {
			/* Still bytes to read ? */
			sr |= (1<<ACIA_SR_RXFULL);

			ThrowInterrupt();
		} 
	}

	SDL_UnlockMutex(rwLock);

	return rxdr;
}

inline uint8 IKBD::int2bcd(int a)
{
	return (a % 10) + ((a / 10) << 4);
}

void IKBD::WriteData(uae_u8 value)
{
	if (outbuffer==NULL)
		return;

	SDL_LockMutex(rwLock);

	outbuffer[outwrite++] = txdr = value;
	D(bug("ikbd: WriteData(0x%02x)",txdr));

#if 0
	printf("ikbd: ");
	for (int i=0;i<outwrite;i++) {
		printf("0x%02x ",outbuffer[i]);			
	}
	printf("\n");
#endif

	switch (outwrite) {
		case 1:
			switch(outbuffer[0]) {
				case 0x08:
					D(bug("ikbd: Set mouse relative mode"));
					mouse_enabled = SDL_TRUE;
					mouserel_enabled = SDL_TRUE;
					joy_enabled[1] = SDL_TRUE;
					outwrite = 0;
					break;
				case 0x0d:
					D(bug("ikbd: Get absolute mouse position"));
					outwrite = 0;
					break;
				case 0x0f:
					D(bug("ikbd: Set Y origin bottom"));
					outwrite = 0;
					break;
				case 0x10:
					D(bug("ikbd: Set Y origin top"));
					outwrite = 0;
					break;
				case 0x11:
					D(bug("ikbd: Resume transfer"));
					outwrite = 0;
					break;
				case 0x12:
					D(bug("ikbd: Disable mouse"));
					mouse_enabled = SDL_FALSE;
					outwrite = 0;
					break;
				case 0x13:
					D(bug("ikbd: Pause transfer"));
					outwrite = 0;
					break;
				case 0x14:
					D(bug("ikbd: Joystick autoread on"));
					outwrite = 0;
					break;
				case 0x15:
					D(bug("ikbd: Joystick autoread off"));
					outwrite = 0;
					break;
				case 0x16:
					D(bug("ikbd: Read joystick"));
					{
						// send a joystick packet to make GFA Basic happy
						intype = IKBD_PACKET_JOYSTICK;
						send(0xfe | 1);
						send(joy_state[1]);
					}
					outwrite = 0;
					break;
				case 0x18:
					D(bug("ikbd: Joystick fire read continuously"));
					outwrite = 0;
					break;
				case 0x1a:
					D(bug("ikbd: Joysticks disabled"));
					joy_enabled[0] = joy_enabled[1] = SDL_FALSE;
					outwrite = 0;
					break;
				case 0x1c:
					{
						D(bug("ikbd: Read date/time"));

						// Get current time
						time_t tim = time(NULL);
						struct tm *curtim = localtime(&tim);	// current time

						// Return packet
						send(0xfc);

						// Return time-of-day clock as yy-mm-dd-hh-mm-ss as BCD
						send(int2bcd(curtim->tm_year % 100));
						send(int2bcd(curtim->tm_mon+1));
						send(int2bcd(curtim->tm_mday));
						send(int2bcd(curtim->tm_hour));
						send(int2bcd(curtim->tm_min));
						send(int2bcd(curtim->tm_sec));

						outwrite = 0;
					}
					break;
			}
			break;
		case 2:
			switch(outbuffer[0]) {
				case 0x07:
					D(bug("ikbd: Set mouse report mode"));
					outwrite = 0;
					break;
				case 0x17:
					D(bug("ikbd: Joystick position read continuously"));
					outwrite = 0;
					break;
				case 0x80:
					if (outbuffer[1]==0x01) {
						reset();
						outwrite = 0;
					}
					break;
			}
			break;
		case 3:
			switch(outbuffer[0]) {
				case 0x0a:
					D(bug("ikbd: Set mouse emulation via keyboard"));
					mouse_enabled = SDL_TRUE;
					joy_enabled[1] = SDL_TRUE;
					outwrite = 0;
					break;
				case 0x0b:
					D(bug("ikbd: Set mouse threshold"));
					outwrite = 0;
					break;
				case 0x0c:
					D(bug("ikbd: Set mouse accuracy"));
					outwrite = 0;
					break;
				case 0x20:
					D(bug("ikbd: Write to IKBD ram"));
					/* FIXME: we should ignore a given number of following bytes */
					outwrite = 0;
					break;
				case 0x21:
					D(bug("ikbd: Read from IKBD ram"));
					outwrite = 0;
					break;
			}
			break;
		case 4:
			switch(outbuffer[0]) {
				case 0x09:
					D(bug("ikbd: Set mouse absolute mode"));
					mouse_enabled = SDL_TRUE;
					mouserel_enabled = SDL_FALSE;
					joy_enabled[1] = SDL_TRUE;
					outwrite = 0;
					break;
			}
			break;
		case 5:
			switch(outbuffer[0]) {
				case 0x0e:
					D(bug("ikbd: Set mouse position"));
					outwrite = 0;
					break;
			}
			break;
		case 6:
			switch(outbuffer[0]) {
				case 0x19:
					D(bug("ikbd: Set joystick keyboard emulation"));
					outwrite = 0;
					break;
				case 0x1b:
					D(bug("ikbd: Set date/time"));
					outwrite = 0;
					break;
			}
			break;
		default:
			outwrite = 0;
			break;
	}

	SDL_UnlockMutex(rwLock);
}

/*--- Functions called to transmit an input event from host to IKBD ---*/

void IKBD::SendKey(uae_u8 scancode)
{
	intype = IKBD_PACKET_KEYBOARD;
	send(scancode);
}

void IKBD::SendMouseMotion(int relx, int rely, int buttons)
{
	if (!mouse_enabled)
		return;

	/* Generate several mouse packets if motion too long */
	while (relx || rely || (buttons!=-1)) {
		int movex, movey;
		
		movex = relx;
		if (movex<-128) {
			movex=-128;
			relx += 128;
		} else if (movex>127) {
			movex=127;
			relx -= 127;
		} else {
			relx = 0;
		}

		movey = rely;
		if (movey<-128) {
			movey=-128;
			rely += 128;
		} else if (movey>127) {
			movey=127;
			rely -= 127;
		} else {
			rely = 0;
		}

		/* Merge with the previous mouse packet ? */
		SDL_LockMutex(rwLock);
		MergeMousePacket(&movex, &movey, buttons);
		SDL_UnlockMutex(rwLock);

		/* Send the packet */
		intype = IKBD_PACKET_MOUSE;
		send(0xf8 | (buttons & 3));
		send(movex);
		send(movey);

		buttons = -1;
	}
}

void IKBD::MergeMousePacket(int *relx, int *rely, int buttons)
{
	int mouse_inwrite;
	int distx, disty;

	/* Check if it was a mouse packet */
	if (intype != IKBD_PACKET_MOUSE)
		return;

	/* And ReadData() is not waiting our packet */
	if (inread == inwrite)
		return;

	/* Where is the previous mouse packet ? */
	mouse_inwrite = (inwrite-3) & (inbufferlen-1);

	/* Check if not currently read */
	if ((inread == mouse_inwrite) ||
		(inread == ((mouse_inwrite+1) & (inbufferlen-1))) ||
		(inread == ((mouse_inwrite+2) & (inbufferlen-1))) )
		return;

	/* Check mouse packet header */
	if ((inbuffer[mouse_inwrite]<=0xf8) && (inbuffer[mouse_inwrite]>=0xfb))
		return;

	/* Check if same buttons are pressed */
	if (inbuffer[mouse_inwrite] != (0xf8 | (buttons & 3)))
		return;

	/* Check if distances are not too far */
	distx = *relx + inbuffer[(mouse_inwrite+1) & (inbufferlen-1)];
	if ((distx<-128) || (distx>127))
		return;

	disty = *rely + inbuffer[(mouse_inwrite+2) & (inbufferlen-1)];
	if ((disty<-128) || (disty>127))
		return;

	/* Replace previous packet */
	inwrite = mouse_inwrite;
	*relx = distx;
	*rely = disty;
}

void IKBD::SendJoystickAxis(int numjoy, int numaxis, int value)
{
	uae_u8 newjoy_state;

	if (!joy_enabled[numjoy])
		return;

	newjoy_state = joy_state[numjoy];

	switch(numaxis) {
		case 0:
			newjoy_state &= ~((1<<IKBD_JOY_LEFT)|(1<<IKBD_JOY_RIGHT));
			if (value<-JOYSTICK_THRESHOLD) {
				newjoy_state |= 1<<IKBD_JOY_LEFT;
			}
			if (value>JOYSTICK_THRESHOLD) {
				newjoy_state |= 1<<IKBD_JOY_RIGHT;
			}
			break;
		case 1:
			newjoy_state &= ~((1<<IKBD_JOY_UP)|(1<<IKBD_JOY_DOWN));
			if (value<-JOYSTICK_THRESHOLD) {
				newjoy_state |= 1<<IKBD_JOY_UP;
			}
			if (value>JOYSTICK_THRESHOLD) {
				newjoy_state |= 1<<IKBD_JOY_DOWN;
			}
			break;
	}

	if (joy_state[numjoy] != newjoy_state) {
		joy_state[numjoy] = newjoy_state;
		intype = IKBD_PACKET_JOYSTICK;
		send(0xfe | numjoy);
		send(joy_state[numjoy]);
	}
}

void IKBD::SendJoystickButton(int numjoy, int pressed)
{
	uae_u8 newjoy_state;

	if (!joy_enabled[numjoy])
		return;

	newjoy_state = joy_state[numjoy];

	newjoy_state &= ~(1<<IKBD_JOY_FIRE);
	newjoy_state |= (pressed & 1)<<IKBD_JOY_FIRE;

	if (joy_state[numjoy] != newjoy_state) {
		joy_state[numjoy] = newjoy_state;
		intype = IKBD_PACKET_JOYSTICK;
		send(0xfe | numjoy);
		send(joy_state[numjoy]);
	}
}

/*--- Queue manager, host side ---*/

void IKBD::send(uae_u8 value)
{
	if (inbuffer == NULL)
		return;

	SDL_LockMutex(rwLock);

	/* Add new byte to IKBD buffer */
	D(bug("ikbd: send(0x%02x) at position %d",value,inwrite));
	inbuffer[inwrite++] = value;

	inwrite &= (inbufferlen-1);
	
	if (inread == inwrite) {
		/* Queue full, need cleanup */
		switch (inbuffer[inread]) {
			/* Remove a packet */
			case 0xf6:
				inread+=8;
				break;
			case 0xf7:
				inread+=6;
				break;
			case 0xf8:
			case 0xf9:
			case 0xfa:
			case 0xfb:
				inread+=3;
				break;
			case 0xfc:
				inread+=7;
				break;
			case 0xfe:
			case 0xff:
				inread+=2;
				break;
			/* Remove a key press/release */
			default:
				inread++;
				break;
		}
		inread &= (inbufferlen-1);
	}

	sr |= (1<<ACIA_SR_RXFULL);

	ThrowInterrupt();

	SDL_UnlockMutex(rwLock);
}

void IKBD::ThrowInterrupt(void)
{
	/* Check MFP IER */
	if ((HWget_b(0xfffa09) & 0x40)==0)
		return;

	/* Check ACIA interrupt on reception */
	if ((cr & (1<<ACIA_CR_REC_INTER))==0)
		return;
		
	/* set Interrupt Request */
	sr |= (1<<ACIA_SR_INTERRUPT);

	/* signal ACIA interrupt */
	mfp.setGPIPbit(0x10, 0);
}
