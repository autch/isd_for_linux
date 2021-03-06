#include "piecedev.h"
#include <stdint.h>
#include <cstring>
#include <cstdio>
#include "debug.h"

#include <unistd.h>

#define USB_ID_PRODUCT 0x1000
#define USB_ID_VENDOR 0x0e19

#define PIECE_END_POINT_IN 0x82
#define PIECE_END_POINT_OUT 0x02
#define BULK_WAIT 500

namespace n {
namespace piece {

namespace {

class Init
{
public:
	Init() {
		int r = libusb_init( NULL );
		if ( r < 0 ) {
			throw libusb_strerror( static_cast<enum libusb_error>(r) );
		}
	}

	~Init() {
		libusb_exit( NULL );
	}
};

Init init;

}

UsbDevHandle::UsbDevHandle()
{
	libusb_device_handle *udev;
	udev = libusb_open_device_with_vid_pid( NULL, USB_ID_VENDOR, USB_ID_PRODUCT );
	if ( udev == NULL )
		throw "can't find device";
	usb_dev_ = udev;
}

UsbDevHandle::~UsbDevHandle()
{
	libusb_reset_device( usb_dev_ );
  libusb_close( usb_dev_ );
}

Device::Device()
	:mblk_adr_(pffs_top_)
{
	int r = libusb_set_configuration( usb_dev_, 1 );
	if ( r < 0 )
		throw libusb_strerror( static_cast<enum libusb_error>(r) );

	r = libusb_claim_interface( usb_dev_, 0 );
	if ( r < 0 )
		throw libusb_strerror( static_cast<enum libusb_error>(r) );

	readVersion();
}

Device::~Device()
{
	libusb_release_interface( usb_dev_, 0 );
}

void Device::readVersion()
{
	char tmp[4];
	char info[32];

	tmp[0] = 0;
	tmp[1] = 32;

	write( tmp, 2 );
	read( info, 32 );

	uint32_t size = *((uint16_t*)(info));
	if( size != 32 )
		throw "unexpected size of system info";
	hard_ver_ = *((uint16_t*)(info+2));
	bios_ver_ = *((uint16_t*)(info+4));
	bios_date_ = *((uint16_t*)(info+6));
	sys_clock_ = *((uint32_t*)(info+8));
	vdde_voltage_ = *((uint16_t*)(info+12));
	sram_top_ = *((uint32_t*)(info+16));
	sram_end_ = *((uint32_t*)(info+20));
	pffs_top_ = *((uint32_t*)(info+24));
	pffs_end_ = *((uint32_t*)(info+28));
}

void Device::write( const char *buf, size_t len, int timeout )
{
	unsigned char *data = reinterpret_cast<unsigned char*>(const_cast<char*>(buf));
	int actual_length = 0;
	int r = libusb_bulk_transfer( usb_dev_, PIECE_END_POINT_OUT
		, data, len, &actual_length, timeout );
	if ( r < 0 )
		throw libusb_strerror( static_cast<enum libusb_error>(r) );
}

void Device::read( char *buf, size_t len, int timeout )
{
	unsigned char *data = reinterpret_cast<unsigned char*>(buf);
	int actual_length = 0;
	int r = libusb_bulk_transfer( usb_dev_, PIECE_END_POINT_IN
		, data, len, &actual_length, timeout );
	if ( r < 0 )
		throw libusb_strerror( static_cast<enum libusb_error>(r) );
}

void Device::readMem( uint32_t begin, char *buf, size_t len )
{
	char tmp[10];
	tmp[0] = READ_MEM;
	*(uint32_t*)(tmp+1) = begin;
	*(uint32_t*)(tmp+5) = len;

	write( tmp, 9 );
	read( buf, len );
}

void Device::writeMem( uint32_t begin, const char *buf, size_t len )
{
	char tmp[10];
	tmp[0] = 3;
	*(uint32_t *)(tmp+1) = begin;
	*(uint32_t *)(tmp+5) = len;

	write( tmp, 9 );
	write( buf, len );
}

bool Device::doWriteFlash( uint32_t rom_adr, uint32_t buf_adr, size_t len )
{
	char tmp[16];

	tmp[0] = DO_FLASH_WRITE;
	*(uint32_t *)(tmp+1) = rom_adr;
	*(uint32_t *)(tmp+5) = buf_adr;
	*(uint32_t *)(tmp+9) = len;

	write( tmp, 13 );
	read( tmp, 2 );

	if ( (*(uint16_t*)tmp) == 0 )
		return true;
	return false;
}

bool Device::eraseFlash( uint32_t adr )
{
	char tmp[8];
	tmp[0] = ERASE_FLASH;
	*(uint32_t*)(tmp+1) = adr;

	write( tmp, 5 );
	read( tmp, 2 );

	if ( *(uint16_t*)tmp == 0 )
		return true;

	return false;
}

bool Device::writeFlash( uint32_t rom_adr, const char *buf, size_t len, bool safe )
{
	DEBUG_MSG( "rom_adr: %x, len: %d\n", rom_adr, len );

	if ( ( safe
	       && rom_adr<(pffs_top_+4096) )
	     || rom_adr>pffs_end_ ) {
		fprintf( stderr, "%x -> unsafe write flash\n", rom_adr);
		return false;
	}

	writeMem( 0x130000, buf, len );
	if ( eraseFlash( rom_adr ) == false )
		return false;
	bool ret = doWriteFlash( rom_adr, 0x130000, len );

	return ret;
}

namespace {

void milisleep( int mili ) {
	usleep( mili*1000 );
}

}

int Device::getAppStat( )
{
	char tmp[4];
	tmp[0] = GET_APPSTAT;

	write( tmp, 1 );
	read( tmp, 2 );

	return *(short*)tmp;
		
}

void Device::setAppStat( int stat )
{
	char tmp[4];
	tmp[0] = SET_APPSTAT;
	tmp[1] = stat;

	int next;

	if ( stat == 3 )
		next = 0;
	else if ( stat == 1 )
		next = 2;
	else
		return;

	write( tmp, 2 );

	for ( int i=0; i<100; i++ ) {
		int now;
		milisleep( 20 );
		now = getAppStat( );
		if ( now==next )
			return;
	}

	throw "appstat timeout";
}

void Device::dumpVersion( )
{
	std::printf( "BIOS version = %d.%02d\n", bios_ver_ >> 8, bios_ver_ & 0xFF );
	std::printf( "BIOS date    = %d.%02d.%02d\n", 2000 + ( bios_date_ >> 9 )
		, ( bios_date_ >> 5 ) & 0x0F, bios_date_ & 0x1F );
	std::printf( "SRAM top adr = 0x%06x\n", sram_top_ );
	std::printf( "SRAM end adr = 0x%06x\n", sram_end_ - 1 );
	std::printf( "SRAM size    = %d KB\n", ( sram_end_ - sram_top_ ) >> 10 );
	std::printf( "HW version   = %d.%02d\n", hard_ver_ >> 8, hard_ver_ & 0xFF );
	std::printf( "SYSTEM clock = %5.3f MHz\n", sys_clock_ / 1e6 );
	std::printf( "VDDE voltage = %5.3f V\n", vdde_voltage_ / 1e3 );
	std::printf( "PFFS top adr = 0x%06x\n", pffs_top_ );
	std::printf( "PFFS end adr = 0x%06x\n", pffs_end_ - 1 );
}

}
}
