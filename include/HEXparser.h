#ifndef HEXparser_h
#define HEXparser_h
#include "Arduino.h"
#define PAGE_LENGTH1 256

class HEXparser
{
public:
	HEXparser();
	void ParseRecord(uint8_t *data); // Import data from spiffs.open() in web server.
	uint8_t *FetchRaw();			 // will be called from main server code
	uint8_t *FetchAddress();		 // for flashing
	bool CheckReady();				 // if flash-ready
									 // uint32_t getHexValue(uint8_t *buf, short len);

private:
	int itercount = 0;
	int pglen = 0;
	int recType = 0; // Here only 0/1 is needed-data present/end of file.
	int length = 0;
	int index = 0;		  // to keep track of the no. of bytes in each chunk
	uint8_t RawPage[300]; // used to store data for page chunk
	bool arrear = false;  // are there any bytes that needs to be added to coming page
	uint8_t overPg[50];	  // bytes that are excess for current page ,, will be moved to the coming page
	int overPgLen = 0;	  // to keep track of the no. of bytes excess last page

	uint8_t address[2];	   // storage array
	bool firstTime = true; // data address is to be updated only after the first chunk transfer
	bool ready = false;	   // init- not ready

	/* Intel HEX file format info */
	int RecordLength(uint8_t *record);
	void RecordAddress(uint8_t *record);
	int RecordType(uint8_t *record);
	//                             //
	void extractData(uint8_t *record, int len);
	void FileEnd();
};

#endif
