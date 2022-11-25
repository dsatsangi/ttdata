#include "UploadProtocol.h"
#include "Arduino.h"
#include "HEXparser.h"

UploadProtocol::UploadProtocol(Stream *serialport, int reset)
{
	_streamRef = serialport;
	pinMode(reset, OUTPUT);
	_reset = reset;
	OPTI_RES = false;
	IN_SYNC = false;
	IN_PGM = false;
	lock4 = false;
}

// initiation functions to be flash-ready
int UploadProtocol::DeviceSetup()
{

	// reset();
	int ti = 0;
	while (!IN_SYNC && ti < 50)
	{
		sync();
		ESP.wdtFeed();
		ti++;
	}

	if (!IN_SYNC)
	{
		return 0;
	}

	ti = 0;
	while (!IN_PGM && ti < 50)
	{
		startProgMode();
		ti++;
	}

	if (!IN_PGM)
	{

		return 0;
	}

	if (IN_PGM)
		return 1;

	return 0;
}

// just toggle it
void UploadProtocol::reset()
{

	digitalWrite(_reset, LOW); // sends a brief reset pulse
	completion_time = millis() + 50;
	while (millis() < completion_time)
	{
	}							// 50ms works better
	digitalWrite(_reset, HIGH); // clears reset
	completion_time = millis() + 50;
	while (millis() < completion_time)
	{
	}						   // ~50ms later
	_streamRef->write("\x20"); // sends CRC_EOP+CRC_EOP
	completion_time = millis() + 70;
	while (millis() < completion_time)
	{
	} // ~70ms later
}

// send command to sync with the chip
void UploadProtocol::sync()
{
	if (!OPTI_RES)
	{
		while (_streamRef->available())
		{
			test1 += String(_streamRef->read()); // check for responce with STK_INSYNC+STK_OK (0x14;0x10)
		}
		if (test1 == "2016")
		{ // eventually optiboot responds with STK_INSYNC+STK_OK (0x14;0x10)
			OPTI_RES = true;
		}
		if (!OPTI_RES)
		{
			_streamRef->write("\x30\x20");
			completion_time = millis() + 70;
			while (millis() < completion_time)
			{
				ESP.wdtFeed();
			}
		}
	}
	else
	{
		if (!IN_SYNC)
		{
			while (_streamRef->available())
			{
				test2 += String(_streamRef->read());
			}
			if (test2 == "2016")
			{
				IN_SYNC = true;
			}
			if (!IN_SYNC)
			{
				_streamRef->write("\x20"); // send one CRC_EOP to sort out the even/odd issue
				completion_time = millis() + 70;
				while (millis() < completion_time)
				{
				}
			}
		}
	}
}

// send command to go into programmable mode
int UploadProtocol::startProgMode()
{
	String test3;
	if (OPTI_RES)
	{
		if (IN_SYNC)
		{
			if (!IN_PGM)
			{
				while (_streamRef->available())
				{
					test3 += String(_streamRef->read());
				}
				if (test3 == "2016")
				{
					IN_PGM = true;
				}
				if (!IN_PGM)
				{
					_streamRef->write("\x50\x20");
					completion_time = millis() + 70;
					while (millis() < completion_time)
					{
					}
				}
			}
			else
			{
				return 1;
			}
		}
	}
	return 1;
}

// get data input from HEX parser and flash it, chunk by chunk.
int UploadProtocol::ProgramPage(uint8_t *address, uint8_t *data)
{
	size_t f = 0;
	
	uint8_t init[] = {0x64, 0x01, 0x00, 0x46};

	setLoadAddress(address[1], address[0]);
	//Serial.print("ProgramPage->address");
	//Serial.println(*address,HEX);
	
	_streamRef->write(init, 4);
	
	for (int i = 0; i < PAGE_LENGTH1; i++)
	{
		f += _streamRef->write(data[i]);
		//Serial.print(data[i],HEX);
		if(f%16==0)	
		{
			//Serial.println();
		}	
	}
		
	_streamRef->write(0x20); // SYNC_CRC_EOP
	//Serial.println("page written");
	ESP.wdtFeed();
	if(WaitBruh(2, 1000))
	{
		//Serial.print(F("true after wring page :"));		
	}
	uint8_t inSync = _streamRef->read();
	//
	//
	uint8_t checkOK = _streamRef->read();
	//
	//
	if (inSync == 0x14 && checkOK == 0x10)
	{
		return 1;
	}
	return 0;
}

// where to write?
int UploadProtocol::setLoadAddress(uint8_t high, uint8_t low)
{
	uint8_t buffer[] = {high, low};
	return SendParams(0x55, buffer, sizeof(buffer));
}

// Function to send commands
uint8_t UploadProtocol::SendCmmnd(uint8_t cmmnd)
{

	uint8_t uint8_ts[] = {cmmnd, 0x20};
	return Writeuint8_ts(uint8_ts, 2);
}

// Function to send parameters
uint8_t UploadProtocol::SendParams(uint8_t cmmnd, uint8_t *params, int len)
{

	uint8_t uint8_ts[32];
	uint8_ts[0] = cmmnd;

	int i = 0;
	while (i < len)
	{
		uint8_ts[i + 1] = params[i];
		i++;
	}

	uint8_ts[i + 1] = 0x20;

	return Writeuint8_ts(uint8_ts, i + 2);
}

// To actually send uint8_ts for command/parameter sending functions
uint8_t UploadProtocol::Writeuint8_ts(uint8_t *uint8_ts, int len)
{
	_streamRef->write(uint8_ts, len);

	WaitBruh(2, 1000);

	uint8_t inSync = _streamRef->read();
	uint8_t checkOK = _streamRef->read();
	if (inSync == 0x14 && checkOK == 0x10)
	{
		return 1;
	}
	return 0;
}

// After flashing is done, exit programming mode!
int UploadProtocol::closeProgMode()
{
	return SendCmmnd(0x51);
}

bool UploadProtocol::WaitBruh(int count, int timeout)
{
	int timer = 0;
	while (timer < timeout)
	{
		if (_streamRef->available() >= count)
		{
			return true;
		}

		completion_time = millis() + 1;
		while (millis() < completion_time)
			;
		timer++;
	}
	return false;
}

bool UploadProtocol::_waitOptibootRes_1s()
{
	bool _is_timeout = false;
	uint32_t _start_time = millis();
	while ((_streamRef->available() < 2) && !(_is_timeout = ((millis() - _start_time) > 1000UL)))
		;
	if (_is_timeout || (_streamRef->read() != 0x14) || (_streamRef->read() != 0x10))
	{
		return false;
	}

	return true;
}
