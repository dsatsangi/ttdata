#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>
#include <Ch376msc.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include <EEPROM.h>
#include <StreamUtils.h>
#include "HEXparser.h"
#include "UploadProtocol.h"

using namespace std;

bool debug = true;           // false = wait for  config msg on boot to sate date
bool Format_on_boot = false; // Format eeprom on boot, true when changes too config save/load code
/******************************************
         Configuration Defaults
******************************************/

const char *AP_SSID = "DataLink_";
const char *AP_PASS = "datalink";

String DEFAULT_SSID = AP_SSID;
String ESPID = String(ESP.getChipId()); // ESP chip id

/* FIXME: login using user and pass */
#define DEFAULT_LOGIN_USERID "user"
#define DEFAULT_LOGIN_PASSWORD "pass"

#define MAX_TELNET_CHAR 200

String tcpDASIP = "192.168.4.2";
int tcpDASPort = 3000;

// InfluxDB v2 server url, e.g. https://eu-central-1-1.aws.cloud2.influxdata.com (Use: InfluxDB UI -> Load Data -> Client Libraries)
String INFLUXDB_URL = "https://europe-west1-1.gcp.cloud2.influxdata.com";

// InfluxDB v2 server or cloud API token (Use: InfluxDB UI -> Data -> API Tokens -> Generate API Token)
String INFLUXDB_TOKEN = "your db token";

// InfluxDB v2 organization id (Use: InfluxDB UI -> User -> About -> Common Ids )
String INFLUXDB_ORG = "test@company.com";

// InfluxDB v2 bucket name (Use: InfluxDB UI ->  Data -> Buckets)
String INFLUXDB_BUCKET = "datalink";

// Set timezone string according to https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
// Examples:
//  Pacific Time: "PST8PDT"
//  Eastern: "EST5EDT"
//  Japanesse: "JST-9"
//  Central Europe: "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_INFO "PST8PDT"

InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);

// Data point
Point sensor("Daslink");

String Date = "18-08-22";
String Time = "00:00";

int flag = 0;
bool first = true;
bool db_con = false;
String pgmstatus;

/******************************************
         end Defaults
******************************************/

typedef struct
{
  WiFiMode_t mode;
  int hidden;
  String network;
  String net_pass;
  String apname;
  String appass;
  String status;
  String address;
  String mac;
  String dasip;
  int dasport;
  String dasurl;
  String dastoken;
  String dasorg;
  String dasbucket;
  String date;
  String time;
} wifi_info_t;

/******************************************
         Ch376 related
******************************************/

SoftwareSerial testUARD1(4, 5); // Read on UART1
Ch376msc flashDrive(testUARD1);

/******************************************
         AVR MCU OTA RELATED
******************************************/
int timeout = WDTO_8S;
bool avr_flash = false;

#define AVR_RESET 14 // pin to reset optiboot D5

#define ESP_RX 12 // connect to TX pin of AVR MCU D6
#define ESP_TX 13 // connect to RX pin of AVR MCU D7

SoftwareSerial avrserial(ESP_RX, ESP_TX); // for AVR MCU
/******************************************
         Services related
******************************************/

AsyncWebServer server(80);
wifi_info_t wi;

WiFiServer telnet_server(23);
WiFiClient telnet_client;

bool is_telnet_client_connected = false;
bool is_login_success = false;

String Rec_data; // used to store body contect from requests

void set_date()
{

  Serial.println("Waiting for Setup msg [Json]");
  Serial.println("[Example] \"{\"date\": \"12-12-30\", \"time\": \"23:05\"}");

  if (!debug)
  {
    while (Serial.available() == 0)
    {
    } // wont go any further until Setup msg is Recieved
  }
  if (Serial.available() > 0)
  {

    String inString = Serial.readStringUntil('\n');

    StaticJsonDocument<100> setup_msg;
    DeserializationError error;

    error = deserializeJson(setup_msg, inString);
    if (error)
    {
      Serial.println("Failed parsing Setup msg");
      wi.date = Date;
      wi.time = Time;
    }
    else
    {
      wi.date = setup_msg["date"].as<String>();
      wi.time = setup_msg["time"].as<String>();
    }
    Serial.println("[Date] " + wi.date);
    Serial.println("[Time] " + wi.time);
  }
}

void format_eeprom()
{
  Serial.println("started formating");
  EEPROM.begin(512);
  for (int i = 0; i < 512; i++)
  {
    EEPROM.write(i, 0xff);
  }
  EEPROM.commit();
  EEPROM.end();
  Serial.println("formating complete");
  flag = 0; // clear flag
}

String get_mode_str(WiFiMode_t mode)
{
  String modes[] = {"NULL", "STA", "AP", "AP+STA"};
  return modes[mode];
}

DynamicJsonDocument read_config(String filename)
{
  DynamicJsonDocument fjson(512);
  DeserializationError error;
  File jfile = LittleFS.open("/" + filename, "r"); // open wifi setting file
  if (jfile)
  {
    error = deserializeJson(fjson, jfile); // read the config file and conver to json
    jfile.close();                         // if we load config file its not the first time on this hardware
  }
  else
  {
    Serial.println("404: " + filename);
  }

  if (!error)
  {
    return fjson;
  }
  else
  {
    fjson.clear();
    return fjson;
  }
}

// bool write_config(filename, json to write)
bool write_config(String filename, const JsonObject &json)
{
  File jfile = LittleFS.open("/" + filename, "w"); // open wifi setting file
  size_t i = serializeJson(json, jfile);           // and write back that edited json to file
  jfile.close();                                   // if we load config file its not the first time on this hardware
  if (i > 0)
  {
    return true;
  }
  else
  {
    return false;
  }
}

void wifi_config(bool read, int mode = 2, String ssid = emptyString, String pass = emptyString, int hidden = 0)
{

  DynamicJsonDocument settings_json(512);
  bool error = false;

  settings_json = read_config("settings.json");

  if (settings_json["mode"].as<int>() == 0)
  {
    EEPROM.begin(512);
    EepromStream eepromStream(0, 256);
    error = deserializeJson(settings_json, eepromStream);
    EEPROM.end();
  }

  if (settings_json["mode"].as<int>() == 0)
  {
    error = true;
  }

  if (settings_json["mode"].as<int>() > 0 && settings_json["mode"].as<int>() <= 3) // heck if config is valid
  {
    first = false; // its not the first time we boot on this device
    error = false;
    Serial.print("settings_json read: ");
    Serial.println(settings_json.as<String>());
  }

  if (read && !error) // if we reading and no error and not the first time i.e config loaded successfully
  {
    Serial.println("Success : Config Loaded");

    wi.mode = (WiFiMode_t)settings_json["mode"].as<int>(); // we get fields from json
    wi.network = settings_json["sta_ssid"].as<String>();
    wi.apname = settings_json["ap_ssid"].as<String>();
    wi.net_pass = settings_json["sta_pass"].as<String>();
    wi.appass = settings_json["ap_pass"].as<String>();
    wi.hidden = settings_json["hidden"].as<int>();

    if (wi.mode == WIFI_STA && wi.network.length() == 0) // if mode sta and ssid empty it wont connect and gets stuck to avoid that change mode to sta
    {
      wi.mode = WIFI_AP;
    }

    WiFi.mode(wi.mode);

    if (wi.mode == WIFI_AP)
    {
      WiFi.softAP(wi.apname, wi.appass, 1, wi.hidden); // if it was in AP mode and was set to hidden , it will stay hidden after reboot
      Serial.print("[AP] ");
      Serial.println(wi.apname);
      Serial.print("[PSK] ");
      Serial.println(wi.appass);
      Serial.print("[AP IP] ");
      Serial.println(WiFi.softAPIP());
      Serial.println("[Wifi AP Hidden] " + String(wi.hidden > 0 ? "yes" : "No"));
      return;
    }

    // Print Local IP Address
    Serial.println("[Wifi Mode] " + get_mode_str(wi.mode));
    Serial.println("[Wifi STA SSID] " + wi.network);
    if (wi.net_pass.length() > 0)
      Serial.println("[Wifi STA PSK] " + wi.net_pass);

    if (wi.mode == WIFI_AP_STA)
    {
      Serial.println("[Wifi AP SSID] " + wi.apname);
      if (wi.appass.length() > 0)
        Serial.println("[Wifi AP PSK] " + wi.appass);
      WiFi.softAP(wi.apname, wi.appass, 1, wi.hidden); // if it was in STA+AP mode and was set to hidden , the AP will stay hidden after reboot
      Serial.println("[Wifi AP Hidden] " + String(wi.hidden > 0 ? "yes" : "No"));
    }
    WiFi.begin(wi.network, wi.net_pass);

    Serial.println("[Wifi IP] " + WiFi.localIP().toString());
    return; // connects to network as normal
  }

  if (read && error)
  {

    // we set default
    wi.mode = WIFI_AP;
    WiFi.mode(wi.mode);
    wi.apname = DEFAULT_SSID;
    wi.appass = AP_PASS;
    WiFi.softAP(DEFAULT_SSID, wi.appass);

    Serial.print("[AP] ");
    Serial.println(DEFAULT_SSID);
    Serial.print("[PSK] ");
    Serial.println(AP_PASS);
    Serial.print("[AP IP] ");
    Serial.println(WiFi.softAPIP());
    return;
  }

  Serial.println("state read= " + String(read) + " error= " + String(error) + " first= " + String(first));
  if ((!read && !error) || first) // if we want to write
  {

    if (mode == WIFI_STA)
    {
      settings_json["sta_ssid"] = ssid; // we change the values of fields
      settings_json["sta_pass"] = pass;
    }

    if (mode == WIFI_AP_STA)
    {
      settings_json["sta_ssid"] = ssid; // we change the values of fields
      settings_json["sta_pass"] = pass;
    }

    if (mode == WIFI_AP)
    {
      settings_json["ap_ssid"] = ssid;
      settings_json["ap_pass"] = pass;
    }

    settings_json["mode"] = mode;
    settings_json["hidden"] = hidden;

    bool res, res1;
    res = res1 = false;
    res = write_config("settings.json", settings_json.as<JsonObject>());

    EEPROM.begin(512);
    EepromStream eepromStream(0, 256);
    size_t rd = serializeJson(settings_json, eepromStream);
    if (rd > 0)
    {
      Serial.println("write to eeprom size" + String(rd));
      res1 = true;
    }
    EEPROM.commit();
    EEPROM.end();
    Serial.println("settings.json written :" + settings_json.as<String>());
    Serial.println("json : " + String(res ? "yes" : "No") + " EEPROM : " + String(res1 ? "yes" : "No"));
    first = false;
    return;
  }

  Serial.println("UNKNWON state read= " + String(read) + " error= " + String(error));
}

// bool read : Read or write Wifi Config dependoing on the bool read variable
void tcpdata(bool read = true, String dasurl = emptyString, String dastoken = emptyString, String dasorg = emptyString, String dasbucket = emptyString, String dasip = emptyString, String dasport = emptyString)
{

  StaticJsonDocument<512> tcpdata_json;
  bool error;

  tcpdata_json = read_config("tcpdata.json");

  if (tcpdata_json.isNull())
  {
    error = true;
    Serial.println("no json rerurned");
  }
  else
  {
    error = false;
    Serial.print("tcp json read:");
    Serial.println(tcpdata_json.as<String>());
  }

  if (read)
  {
    if (!error)
    {
      wi.dasip = tcpdata_json["ip"].as<String>();
      wi.dasport = tcpdata_json["port"].as<int>();
      wi.dasurl = tcpdata_json["url"].as<String>();
      wi.dastoken = tcpdata_json["token"].as<String>();
      wi.dasorg = tcpdata_json["org"].as<String>();
      wi.dasbucket = tcpdata_json["bucket"].as<String>();
    }
    else
    {
      Serial.println("[TCP] Usig Default");
      // if file failed to open or does not exist we load defaults
      wi.dasip = tcpDASIP;
      wi.dasport = tcpDASPort;
      wi.dasurl = INFLUXDB_URL;
      wi.dasbucket = INFLUXDB_BUCKET;
      wi.dastoken = INFLUXDB_TOKEN;
      wi.dasorg = INFLUXDB_ORG;
    }
  }

  if (!read)
  {
    if (dasip != emptyString)
      tcpdata_json["ip"] = wi.dasip = dasip;
    else
      tcpdata_json["ip"] = wi.dasip;

    if (dasport != emptyString)
      tcpdata_json["port"] = wi.dasport = dasport.toInt();
    else
      tcpdata_json["port"] = wi.dasport;

    if (dasurl != emptyString)
      tcpdata_json["url"] = wi.dasurl = dasurl;
    else
      tcpdata_json["url"] = wi.dasurl;

    if (dastoken != emptyString)
      tcpdata_json["token"] = wi.dastoken = dastoken;
    else
      tcpdata_json["token"] = wi.dastoken;

    if (dasorg != emptyString)
      tcpdata_json["org"] = wi.dasorg = dasorg;
    else
      tcpdata_json["org"] = wi.dasorg;

    if (dasbucket != emptyString)
      tcpdata_json["bucket"] = wi.dasbucket = dasbucket;
    else
      tcpdata_json["bucket"] = wi.dasbucket;

    Serial.print("tcp json write:");
    Serial.println(tcpdata_json.as<String>());

    if (write_config("tcpdata.json", tcpdata_json.as<JsonObject>())) // write to file
      Serial.println("tcpdata.json written");
  }

  Serial.println("TCP Config Loaded:");
  Serial.println("[DAS IP] " + wi.dasip);
  Serial.println("[DAS PORT] " + String(wi.dasport));
  Serial.println("[DB URL] " + wi.dasurl);
  Serial.println("[DB Token] " + wi.dastoken);
  Serial.println("[DB ORG] " + wi.dasorg);
  Serial.println("[DB Bucket] " + wi.dasbucket);
}

void wifi_settings(int mode, String ssid, String pass, int hidden)
{
  Serial.print("[WiFi] switched to: ");
  switch (mode)
  {
  case 0:
    Serial.println("WIFI MODE NOT SUPPORTED");
    return;

  case 1:
    WiFi.softAPdisconnect();
    Serial.println("Station Mode");
    wi.mode = WIFI_STA;
    wi.network = ssid;
    wi.net_pass = pass;
    WiFi.mode(wi.mode);
    // CONNECT
    WiFi.begin(wi.network, wi.net_pass);
    wifi_config(false, 1, wi.network, wi.net_pass);
    break;

  case 2:
    WiFi.disconnect();
    Serial.println("Access Point Mode");
    wi.mode = WIFI_AP;
    wi.hidden = hidden;
    wi.apname = ssid;
    wi.appass = pass;
    WiFi.softAP(wi.apname, wi.appass, 1, wi.hidden, 4);
    wifi_config(false, 2, wi.apname, wi.appass, wi.hidden);
    break;

  case 3:
    Serial.println("Station & Access Point Mode");
    wi.mode = WIFI_AP_STA;
    WiFi.mode(wi.mode);
    if (wi.apname.length() == 0)
      wi.apname = DEFAULT_SSID;
    wi.network = ssid;
    wi.net_pass = pass;
    wi.hidden = hidden;
    // CONNECT
    WiFi.softAP(wi.apname, wi.appass, 1, wi.hidden, 4);
    WiFi.begin(wi.network, wi.net_pass);
    wifi_config(false, 2, wi.apname, wi.appass, wi.hidden);
    wifi_config(false, 3, wi.network, wi.net_pass, wi.hidden);
    break;
  }

  if (wi.mode == 1 || wi.mode == 3)
  {
    Serial.println("[Wifi STA SSID] " + wi.network);
    if (wi.net_pass.length() > 0)
      Serial.println("[Wifi STA PSK] " + wi.net_pass);
  }

  if (wi.mode == 2 || wi.mode == 3)
  {
    Serial.println("[Wifi AP SSID] " + wi.apname);
    if (wi.appass.length() > 0)
      Serial.println("[Wifi AP PSK] " + wi.appass);
    Serial.println("[Wifi AP Hidden] " + String(wi.hidden > 0 ? "yes" : "No"));
  }

  Serial.println("[Wifi IP] " + WiFi.localIP().toString());
}

void update_wifi_state()
{
  if (wi.mode != WIFI_AP)
    wi.network = WiFi.SSID();

  if (wi.mode == WIFI_AP)
    wi.address = WiFi.softAPIP().toString();
  else
    wi.address = WiFi.localIP().toString();

  if (wi.address != "0.0.0.0")
    wi.status = "got IP address";
  else
    wi.status = "connecting...!";

  wi.mac = WiFi.macAddress();
  // Serial.println(wi.address);
}

void Init_Das()
{

  db_con = false; // diable connetion when changing credentials

  if (wi.mode == WIFI_AP || wi.mode == WIFI_OFF)
  {
    Serial.println("[WiFi] No Internet");
    return;
  }

  if (wi.dasorg == "test@company.com")
  {
    Serial.println("[DB] Invalid credentials");
    return;
  }

  Serial.println("Establishing connection with Database Server");

  client.setConnectionParams(wi.dasurl, wi.dasorg, wi.dasbucket, wi.dastoken, InfluxDbCloud2CACert); // update influxdb credentials

  sensor.clearTags();

  // Add tags
  sensor.addTag("device", ESPID);
  sensor.addTag("SSID", wi.network);

  // Accurate time is necessary for certificate validation and writing in batches
  // For the fastest time sync find NTP servers in your area: https://www.pool.ntp.org/zone/
  // Syncing progress and the time will be printed to Serial.
  struct tm *timeinfo;
  String timeVar;
  int year;

  do
  { // make sure the date we get is correct
    timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");
    time_t tnow = time(nullptr);
    timeinfo = localtime(&tnow);
    // timeVar = ctime(&tnow);
    // year = timeVar.substring(20);
    year = 1900 + timeinfo->tm_year;

    Serial.print("[year] ");
    Serial.println(year);

  } while (2022 > year);

  // wi.date = timeVar.substring(8, 11) + "-" + timeVar.substring(4, 8) + "-" + year.substring(2);
  // wi.date.replace(" ", "");
  wi.date = String(timeinfo->tm_mday) + "-" + String(timeinfo->tm_mon + 1) + "-" + String(timeinfo->tm_year).substring(1);
  Serial.println("[Date] " + wi.date);

  // Check server connection
  if (client.validateConnection())
  {
    Serial.print("[Server] [InfluxDB] OK : ");
    Serial.println(client.getServerUrl());
    db_con = true;
  }
  else
  {
    Serial.print("[Server] [InfluxDB] failed: ");
    Serial.println(client.getLastErrorMessage());
    db_con = false;
  }

  flag = 0; // clear the flag
}

String processor(const String &var)
{
  if (var == "MODE")
    return get_mode_str(wi.mode);
  if (var == "NETWORK")
    return wi.network;
  if (var == "STATUS")
    return wi.status;
  if (var == "ADDRESS")
    return wi.address;
  if (var == "MAC")
    return wi.mac;
  if (var == "DASTCPIP")
    return wi.dasip;
  if (var == "DASTCPPORT")
    return String(wi.dasport);
  if (var == "DASURL")
    return wi.dasurl;
  if (var == "BUCKET")
    return wi.dasbucket;
  if (var == "DASORG")
    return wi.dasorg;
  if (var == "HIDDEN")
    return String(wi.hidden);
  if (var == "ESPID")
    return String(ESPID);

  return String();
}

bool validate_login_credentials(String userid, String pass)
{
  if (userid != DEFAULT_LOGIN_USERID && pass != DEFAULT_LOGIN_PASSWORD)
    return false;
  return true;
}

void handle_post_request_data(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{

  // Serial.println("[URL]" + request->url());

  if (!index)
  {
    // Serial.printf("BodyStart: %u B\n", total);
  }

  for (size_t i = 0; i < len; i++)
  { // accumelate large data
    Rec_data = Rec_data + (char)data[i];
  }

  if (index + len == total)
  { // when data is complete , process accumelated data

    // Serial.printf("BodyEnd: %u B\n", total);
    // Serial.print("[Data]");
    // Serial.println(Rec_data);
    StaticJsonDocument<JSON_OBJECT_SIZE(50)> doc;
    JsonObject root = doc.to<JsonObject>();
    DeserializationError err = deserializeJson(doc, Rec_data);

    if (err)
    {
      // Serial.println("Deserialization: Data Corrupted");
      return;
    }

    if (request->url() == "/savedasinfo")
    {
      String tcpserverip = root["tcpserverip"];
      String tcpserverport = root["tcpserverport"];

      wi.dasip = tcpserverip;
      wi.dasport = tcpserverport.toInt();

      tcpdata(false, emptyString, emptyString, emptyString, emptyString, tcpserverip, tcpserverport);

      request->send(200, "application/json", "{\"status\": \"ok\"}");
    }

    if (request->url() == "/savedbinfo")
    {
      String tcpserverurl = root["url"];
      String tcpservertoken = root["token"];
      String tcpserverorg = root["org"];
      String tcpserverbucket = root["bucket"];
      String tcpserverlocal = root["local"]; //=======TODO

      wi.dasurl = tcpserverurl;
      wi.dastoken = tcpservertoken;
      wi.dasorg = tcpserverorg;
      wi.dasbucket = tcpserverbucket;

      tcpdata(false, wi.dasurl, wi.dastoken, wi.dasorg, wi.dasbucket, emptyString, emptyString);

      flag = 1; // set flag to reconnect to DB in loop

      request->send(200, "application/json", "{\"status\": \"ok\"}");
    }

    if (request->url() == "/settings")
    {
      String ssid = root["ssid"];
      String pass = root["pass"];
      int hidden = root["hidden"].as<int>();
      int mode = root["mode"];

      request->send(200, "application/json", "{\"status\": \"ok\"}");
      wifi_settings(mode, ssid, pass, hidden);
    }

    if (request->url() == "/login")
    {
      String userid = root["userid"];
      String pass = root["pass"];
      String status = "";

      if (validate_login_credentials(userid, pass) == true)
      {
        status = "{\"status\": \"ok\"}";
        is_login_success = true;
      }
      else
      {
        status = "{\"status\": \"not ok\"}";
        is_login_success = false;
      }
      request->send(200, "application/json", status);
    }

    Rec_data = ""; // Clear this storage after processing is done
  }
}

void handle_post_request_update(AsyncWebServerRequest *request)
{
  StaticJsonDocument<JSON_OBJECT_SIZE(150)> doc;
  JsonObject object = doc.to<JsonObject>();
  String data;

  update_wifi_state();

  object["mode"] = get_mode_str(wi.mode);
  object["network"] = wi.network;
  object["status"] = wi.status;
  object["address"] = wi.address;
  object["mac"] = wi.mac;
  object["dasip"] = wi.dasip;
  object["dasport"] = wi.dasport;
  object["dasurl"] = wi.dasurl;
  object["dasbucket"] = wi.dasbucket;
  object["dasorg"] = wi.dasorg;

  serializeJson(doc, data);

  // Serial.println(data);
  request->send(200, "application/json", data.c_str());
}

void notFound(AsyncWebServerRequest *request)
{
  request->send(404, "text/plain", "Not found");
}

void on_telnet_connect()
{
  Serial.println("Client connected");
  telnet_client.println("Welcome to telnet");
}

void on_telnet_disconnect()
{
  Serial.println("Client disconnected");
}

void handle_telnet()
{

  /* Check for clients */
  if (telnet_server.hasClient())
  {
    is_telnet_client_connected = true;
    telnet_client = telnet_server.available();
    on_telnet_connect();
  }

  /* Client disconnected */
  if (is_telnet_client_connected && !(telnet_client.connected()))
  {
    is_telnet_client_connected = false;
    on_telnet_disconnect();
  }
}

void SendDataToServer(String inString)
{
  if (wi.mode == WIFI_AP || wi.mode == WIFI_OFF) // only continue if we are connected to a network
  {
    Serial.println("[WiFi] No Internet");
    return;
  }

  WiFiClient das;
  if (das.connect(wi.dasip, wi.dasport))
  {
    Serial.println("tcpDAS Server connected");
    das.print(inString);
    das.stop();
  }
  else
  {
    Serial.print("failed to connect to tcpDAS Server");
  }

  if (!db_con)
  {
    Serial.println("[DB] Not connected");
    return;
  }

  // Clear fields for reusing the point. Tags will remain untouched
  sensor.clearFields();

  // Store Data recived into point
  sensor.addField("data", inString);

  // Print what are we exactly writing
  Serial.print("  Writing: ");
  Serial.println(sensor.toLineProtocol());

  // Write point

  if (!client.writePoint(sensor))
  {
    Serial.print("InfluxDB write failed: ");
    Serial.println(client.getLastErrorMessage());
  }
  else
  {
    Serial.println("InfluxDB write success: ");
  }
}

bool store(String sdata)
{
  bool state = true;
  String fileName = wi.date + ".TXT";
  if (flashDrive.driveReady())
  {
    flashDrive.setFileName(fileName.c_str());
    if (flashDrive.openFile() == ANSW_USB_INT_SUCCESS)
    {
      flashDrive.moveCursor(CURSOREND);
    }

    if (flashDrive.getFreeSectors()) // check the free space on the drive
    {
      // ch376 testchange
      int len = sdata.length() + 1;
      uint8_t buffer[len];
      sdata.getBytes(buffer, sdata.length());
      buffer[sdata.length()] = '\0';
      flashDrive.writeFile((char *)buffer, len); // string, string length
      flashDrive.closeFile();
      Serial.println("[USB] WriteFile: Success");

      // flashDrive.writeFile((char *)sdata.c_str(), sdata.length()); // string, string length
      // flashDrive.closeFile();
      // Serial.println("Done");
    }
    else
    {
      state = false;
      Serial.println(F("[USB] WriteFile: Disk full"));
    }
  }
  else
  {
    state = false;
    Serial.println(F("[USB] WriteFile: Drive not found or busy"));
  }
  return state;
}

void handle_uart_data()
{
  /*-------------------------------------------------------
    as long as there are bytes in the avrserial queue,
    read them and send them out the socket if it's connected:
    ---------------------------------------------------------*/
//  Serial.println("Avrserial.available(): " + String(avrserial.available()));

  if (avrserial.available() == 0) // only continue if data exist
    return;

  String inString;

  // for (int i = 0; i < datlen; i++)
  //   inString += avrserial.read();

  while (avrserial.available() > 0)
  {
    inString = avrserial.readStringUntil('\n');
  }

  Serial.println("Avrserial Data recived: " + inString);

  SendDataToServer(inString);

  if (telnet_client.connected())
  {
    telnet_client.print(inString);
  }

  if (!flashDrive.driveReady())
  { // if no flash drive are attached
    Serial.println("Attach flash drive first!");
  }
  else
  {
    store(inString);
  }
}

String listdir()
{
  String list = "";
  Dir dir = LittleFS.openDir("/hex");

  while (dir.next())
  {
    list += dir.fileName() + ";";

    File f = dir.openFile("r");
    list += String(f.size()) + ";\n";
  }
  list.concat(String(F("\r\n")));
  return list;
}

void optiboothandle(AsyncWebServerRequest &request)
{
  //   int args = request->args();
  // for(int i=0;i<args;i++){
  //   Serial.printf("ARG[%s]: %s\n", request->argName(i).c_str(), request->arg(i).c_str());
  // }

  String path = "/hex/";

  if (!request.hasParam("cmd"))
  {
    Serial.println("page requested");
    request.send(LittleFS, "/ftp.html", "text/html", false);
  }

  if (request.hasParam("cmd"))
  {
    String cmd = request.getParam("cmd")->value();
    Serial.print("Param : ");
    Serial.print(request.getParam("cmd")->name());
    Serial.print(" | Value : ");
    Serial.println(cmd);

    if (cmd == "list")
    {
      String res = listdir();
      request.send(200, "text/plain", res);
    }

    else if (cmd == "del")
    {
      String filename = "";
      if (request.hasParam("filename"))
        filename = request.getParam("filename")->value();

      if (filename.length() == 0)
      {
        request.send(200, "text/plain", "Please Select a file");
        return;
      }

      bool res = LittleFS.remove(path + filename);
      if (res)
      {
        Serial.print("Deleted : ");
        Serial.println(path + filename);
        request.send(200, "text/plain", listdir());
      }
      else
      {
        Serial.print("Failed to deleted : ");
        Serial.println(path + filename);
        request.send(200, "text/plain", "fail");
      }
    }

    else if (cmd == "flash")
    {
      String filename = "";
      if (request.hasParam("filename"))
      {
        filename = request.getParam("filename")->value();
        Serial.print("file : ");
        Serial.println(filename);

        File file = LittleFS.open(path + filename, "r");
        if (file)
        {
          avr_flash = true; // put avrserial into flash mode i.e stop checking it for data coming from tndatalink

          // avrserial initiallized @ boot . avrota and tndatalink both on avrserial
          /**
          avrserial.begin(115200);
          while (!avrserial) // wait for serial port to connect
          {
            ;
          }
          **/

          UploadProtocol avrdude(&avrserial, AVR_RESET); // Reset pin for Avr MCU

          int ji = 0;
          int num_page=1;
          avrdude.reset();

          ji = avrdude.DeviceSetup();

          if (ji == 1)
          {
            pgmstatus = "MCU READY FLASHING!!.";
            Serial.println(pgmstatus);

            HEXparser parse = HEXparser();
            int opti = 0;

            while (file.available())
            {
              uint8_t buffer[55];
              String data = file.readStringUntil('\n');
              data.getBytes(buffer, data.length());
              parse.ParseRecord(buffer); // parse one record

              if (parse.CheckReady())
              {
                uint8_t *page = parse.FetchRaw();
                uint8_t *address = parse.FetchAddress();
                opti = avrdude.ProgramPage(address, page);
                if (opti == 0)
                  break;
                pgmstatus = String(num_page++)+" Pages Flashed";
              }
            }

            avrdude.closeProgMode();

            file.close();

            if (opti == 1)
            {
              pgmstatus = "MCU Flash complete!!.";
              Serial.println(pgmstatus);
              request.send(200, "text/plain", "MCU FLASHED OK");
            }
            else
            {
              pgmstatus = "MCU Flash Failed!!.";
              Serial.println(pgmstatus);
              request.send(200, "text/plain", "Error occured during flashing");
            }
          }
          else
          {
            pgmstatus = "MCU Setup Failed!!.";
            Serial.println(pgmstatus);
            request.send(200, "text/plain", "MCU Setup Failed!!.");
          }
          avr_flash = false; // diable flash mode of avrserial i.e check if data available.
        }
        else
        {
          pgmstatus = "failed to open " + filename;
          Serial.println(pgmstatus);
          request.send(200, "text/plain", "Failed to open file");
        }
      }
      else
      {
        pgmstatus = "Please Select a file";
        Serial.println(pgmstatus);
        request.send(200, "text/plain", "Please Select a file");
      }
//      pgmstatus = "Flasher Idle";
      Serial.println(pgmstatus);
    }
    else
    {
      request.send(200, "text/plain", "huh?");
    }
  }
}

void optiboot_data(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  if (!index)
  {
    Serial.println((String) "UploadStart: " + filename);
    // open the file on first call and store the file handle in the request object
    String path = "/hex/" + filename;
    request->_tempFile = LittleFS.open(path, "w+");
  }
  if (len)
  {
    // stream the incoming chunk to the opened file
    request->_tempFile.write(data, len);
  }
  if (final)
  {
    Serial.println((String) "UploadEnd: " + filename + "," + index + len);
    // close the file handle as the upload is now done
    request->_tempFile.close();
    request->send(200, "text/plain", "File Uploaded !");
  }
}

void setup()
{
  // String wifiMacString = WiFi.macAddress();

  DEFAULT_SSID.concat(ESPID); // set default AP name with

  Serial.begin(9600); // debug serial

  pinMode(AVR_RESET, OUTPUT);
  digitalWrite(AVR_RESET,HIGH);
  ESP.wdtEnable(timeout);

  Serial.println("[device ID] " + ESPID);

  // Serial.println("[FS]" + (String)((size_t)&_FS_end - (size_t)&_FS_start));
  // Serial.println("[flash free]" + (String)((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000));

  set_date();            // Set date and time using Setup msg recieved on Serial
  testUARD1.begin(9600); // ch376 serial 9600 default
  flashDrive.init();     // ch376 object initialize

  avrserial.begin(9600); // initialize avrserial for tndatalink and can be used later for avrota

  if (Format_on_boot)
    format_eeprom();

  Serial.println("Mounting FS...");
  if (!LittleFS.begin())
  {
    Serial.println("Failed to mount file system");
    return;
  }

  WiFi.persistent(false);
  wifi_config(true); // load wifi config file and setup
  tcpdata(true);     // load tcp and db config file and setup

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(LittleFS, "/index.html", String(), false, processor); });

  server.on("/home", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (is_login_success) {
      request->send(LittleFS, "/home.html", "text/html", false, processor);
    }
    else {
      request->redirect("/");
    } });

  server.on("/format", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (is_login_success) {
      flag=2; //set flag to format eeprom
      Serial.println("formating eeprom");
      request->send(LittleFS, "/home.html", "text/html", false, processor);
    }
    else {
      request->redirect("/");
    } });

  server.on("/wifistation", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (is_login_success) {
      request->send(LittleFS, "/wifistation.html", "text/html", false);
    }
    else {
      request->redirect("/");
    } });

  server.on("/wifisoftap", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (is_login_success) {
      request->send(LittleFS, "/wifisoftap.html", "text/html", false);
    }
    else {
      request->redirect("/");
    } });

  server.on("/dasconfig", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (is_login_success) {
      request->send(LittleFS, "/dasconfig.html", "text/html", false);
    }
    else {
      request->redirect("/");
    } });

  server.on("/dbconfig", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (is_login_success) {
      request->send(LittleFS, "/dbconfig.html", "text/html", false);
    }
    else {
      request->redirect("/");
    } });

  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (is_login_success) {
      request->send(LittleFS, "/update.html", "text/html", false, processor);
    }
    else {
      request->redirect("/");
    } });

  server.on(
      "/pgm", HTTP_POST, [](AsyncWebServerRequest *request)
      { if (!is_login_success) {
      request->redirect("/");
    }
    else {
      request->send(200);
    } },
      optiboot_data);

  server.on("/pgm", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  if (!is_login_success)
  {
  request->redirect("/");
  }
  else {
    optiboothandle(*request);
  } });

  server.on("/pgmstatus", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/html", pgmstatus); });

  server.on(
      "/update", HTTP_POST, [](AsyncWebServerRequest *request)
      {
    if (!is_login_success) {
      request->redirect("/");
    }
    // the request handler is triggered after the upload has finished...
    // create the response, add header, and send response
    AsyncWebServerResponse *response = request->beginResponse((Update.hasError()) ? 500 : 200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    response->addHeader("Connection", "close");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
    // yield();
    ESP.restart(); },
      [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
      {
        // Upload handler chunks in data
        if (!is_login_success)
        {
          request->redirect("/");
        }

        if (!index)
        {
          Serial.println("udate started file: " + filename);

          int cmd = (filename == "littlefs.bin") ? U_FS : U_FLASH;
          Update.runAsync(true);
          size_t fsSize = ((size_t)&_FS_end - (size_t)&_FS_start);
          uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
          if (!Update.begin((cmd == U_FS) ? fsSize : maxSketchSpace, cmd))
          { // Start with max available size
            Update.printError(Serial);
            return request->send(400, "text/plain", "OTA could not begin " + String(Update.getError()));
          }
        }

        // Write chunked data to the free sketch space
        if (len)
        {
          if (Update.write(data, len) != len)
          {
            return request->send(400, "text/plain", "OTA could not begin " + String(Update.getError()));
          }
        }

        if (final)
        { // if the final flag is set then this is the last frame of data
          Serial.println("udate end. Flashing Now then Restarting");
          if (!Update.end(true))
          { // true to set the size to the current progress
            Update.printError(Serial);
            return request->send(400, "text/plain", "Could not end OTA " + String(Update.getError()));
          }
        }
        else
        {
          return;
        }
      });

  server.serveStatic("/static/", LittleFS, "/static/");

  server.onNotFound(notFound);

  /* Handling post request webpage updates */
  server.on("/status", HTTP_POST, handle_post_request_update);

  /* Handling post request recieve data */
  server.onRequestBody(handle_post_request_data);

  server.begin();
  Serial.println("[Server] [HTTP] OK");

  telnet_server.begin();
  telnet_server.setNoDelay(true);

  Serial.println("[Server] [Telnet] OK");

  Init_Das(); // Setup influx DB connection
}

void loop()
{
  if (flashDrive.checkIntMessage())
  { // check USB device
    if (flashDrive.getDeviceStatus())
    {
      Serial.println(F("Flash drive attached!"));
    }
    else
    {
      Serial.println(F("Flash drive detached!"));
    }
  }

  //  handle_telnet();
  if (!avr_flash)
    handle_uart_data();

  if (flag == 1)
    Init_Das(); // if flag set reconnect DB

  if (flag == 2)
    format_eeprom(); // if flag set reconnect DB
}