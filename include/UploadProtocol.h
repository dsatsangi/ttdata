#ifndef UploadProtocol_h
#define UploadProtocol_h
#include "Arduino.h"

class UploadProtocol
{
public:
	UploadProtocol(Stream *streamObject, int reset);
	void reset();
	int DeviceSetup();
	void sync();
	int startProgMode();
	int ProgramPage(uint8_t *address, uint8_t *data);
	int setLoadAddress(uint8_t high, uint8_t low);
	int closeProgMode();

private:
	bool _waitOptibootRes_1s();
	uint8_t SendCmmnd(uint8_t cmmnd);
	uint8_t SendParams(uint8_t cmmnd, uint8_t *params, int len);
	uint8_t Writeuint8_ts(uint8_t *uint8_ts, int len);
	bool WaitBruh(int c, int time);
	Stream *_streamRef;
	int _reset;
	time_t completion_time;

	String test1;
	String test2;
	String test3;
	String test4;

	boolean OPTI_RES;
	boolean IN_SYNC;

	boolean IN_PGM;
	boolean lock4;
};

#endif
