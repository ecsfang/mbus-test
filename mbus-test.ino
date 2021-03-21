#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include "Crc16.h"
#include "secrets.h"
#include "RemoteDebug.h"  //https://github.com/JoaoLopesF/RemoteDebug
#include "kamstrup.h"

HardwareSerial* hanPort;



static const int LED_PIN = LED_BUILTIN ;

#define DLMS_READER_BUFFER_SIZE 1024
byte buffer[DLMS_READER_BUFFER_SIZE+1];
int position = 0;

WiFiClient espClient;
PubSubClient client(espClient);
RemoteDebug Debug;


Energy_t meter;

void blink(int count) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(50);
    digitalWrite(LED_PIN, HIGH);
    delay(50);
  }
}


void setup() {
  
  hanPort = &Serial;
  hanPort->begin(2400, SERIAL_8N1);
  hanPort->swap();
  Serial1.begin(115200);

  pinMode(LED_PIN, OUTPUT);

  setup_wifi();
  client.setServer(mqtt_server, 1883);

  // Initialize the server (telnet or web socket) of RemoteDebug
  Debug.begin("hanReader");
  Debug.printf("Hello from Han-Reader!\n");

  Debug.printf("WiFi connected\nIP address: ");
  Debug.println(WiFi.localIP());

  Debug.printf("Init OTA ...\n");
  ArduinoOTA.setHostname(otaHost);
  ArduinoOTA.setPassword(flashpw);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Debug.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Debug.printf("\nEnd\n");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Debug.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Debug.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Debug.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Debug.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Debug.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Debug.println("End Failed");
    } else {
      Debug.println("Unknown error!");
    }
  });
  ArduinoOTA.begin();

  blink(10);
}

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  WiFi.begin(wifi_ssid, wifi_password);

  while (WiFi.status() != WL_CONNECTED) {
    blink(5);
    Serial1.print(".");
  }
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Debug.printf("Attempting MQTT connection...");
    // Attempt to connect
    // If you do not want to use a username and password, change next line to
    // if (client.connect("ESP8266Client")) {
    if (client.connect("hanMeter", mqtt_user, mqtt_password)) {
      Debug.printf("connected\n");
    } else {
      Debug.printf("failed, rc=%d\n", client.state());
      Debug.printf(" try again in 5 seconds\n");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

// the loop function runs over and over again forever
void loop() {
  while (hanPort->available() > 0) {
    if( ReadData(hanPort->read()) ) {
      CheckMessage(buffer, position);
      blink(2);
    }
  }
  // Remote debug over WiFi
  Debug.handle();
  ArduinoOTA.handle();
  delay(100);
}

/*
  Data should start with E6 E7 00 0F
  and continue with four bytes for the InvokeId
*/
bool isValidHeader(byte *data)
{
  byte validHeader[] = { 0xE6, 0xE7, 0x00, 0x0F };
  return memcmp(data, validHeader, 4) == 0;  
}

bool CheckMessage(byte *buffer, int pos)
{
  byte *userData;
  int userDataLen;
  bool ret = false;
  
  GetUserDataBuffer(userData, userDataLen);
  
  if( !isValidHeader(userData) )
  {
    Debug.printf("Invalid HAN data: Start should be E6 E7 00 0F\n");
    return false;
  }

  //dumpHex(userData,userDataLen);
  
  // So this is where we need to differentiate between Aidon, Kaifa and Kamstrup meters.
  // From what I've gathered Kaifa and Kamstrup shows in an untyped datetime with just
  // the length is a preamble. We still start with +9 position into the PDU.
  int firstStructurePos = 8;

  // If the byte is 0x0C then we can assume that it is Kaifa or Kamstrup
  byte firstByte = userData[firstStructurePos];
  if (firstByte == 0x0C) {
    // I only support Kamstrup ...
    ret = decodeKaifaKamstrupMeter(&userData[firstStructurePos]);
  }
  else {
    Debug.printf("First byte is: 0x%02X\n",firstByte);
  }

  return true;
}

void Clear() {
  position = 0;
}

int dataLength = 0;

#define DLMS_READER_MAX_ADDRESS_SIZE 5
byte frameFormatType;
byte destinationAddress[DLMS_READER_MAX_ADDRESS_SIZE];
byte destinationAddressLength;
byte sourceAddress[DLMS_READER_MAX_ADDRESS_SIZE];
byte sourceAddressLength;

Crc16Class Crc16;

bool checkChecksum(int pos)
{
  // Verify the header checksum
  uint16_t checksum = GetChecksum(pos - 2);
  uint16_t calculatedChecksum = Crc16.ComputeChecksum(buffer, 1, pos - 3);
  return checksum == calculatedChecksum;
}

#define START_OF_FRAME  0x7E

// Receive byte by byte and build the complete buffer
// Return true when a complete buffer is received
bool ReadData(byte data)
{

  if( position == 0 && data != START_OF_FRAME ) {
    // we haven't started yet, wait for the start flag (no need to capture any data yet)
    return false;
  }

  // We have completed reading of one package, so clear and be ready for the next
  if( dataLength > 0 && position >= dataLength + 2 ) {
    Clear();
  }

  // Check if we're about to run into a buffer overflow
  if (position >= DLMS_READER_BUFFER_SIZE) {
    Debug.printf("Buffer overrun!\n");
    Clear();
  }

  // Check if this is a second start flag, which indicates the previous one was a stop from the last package
  if (position == 1 && data == START_OF_FRAME)
  {
    Debug.printf("Second Start: 0x%02X\n", START_OF_FRAME);
    // just return, we can keep the one byte we had in the buffer
    return false;
  }

  // We have started, so capture every byte
  buffer[position++] = data;

  if (position == 1)
  {
    Debug.print("Start frame - ");
    //messageTimestamp = getEpochTime();
    // This was the start flag, we're not done yet
    return false;
  }
  else if (position == 2)
  {
    // Capture the Frame Format Type
    frameFormatType = (byte)(data & 0xF0);
    if (frameFormatType != 0xA0 ) {
      Debug.printf("Invalid frame!\n");
      Clear();
    }
    return false;
  }
  else if (position == 3)
  {
    // Capture the length of the data package
    dataLength = getO16int(buffer+1) & 0xFFF; //((buffer[1] & 0x0F) << 8) | buffer[2];
    return false;
  }
  else if (destinationAddressLength == 0)
  {
    // Capture the destination address
    destinationAddressLength = GetAddress(3, destinationAddress, 0, DLMS_READER_MAX_ADDRESS_SIZE);
    if (destinationAddressLength > 3) {
      Debug.printf(" dst addr > max addr size : %d\n", destinationAddressLength);
      Clear();
    }
    return false;
  }
  else if (sourceAddressLength == 0)
  {
    // Capture the source address
    sourceAddressLength = GetAddress(3 + destinationAddressLength, sourceAddress, 0, DLMS_READER_MAX_ADDRESS_SIZE);
    if (sourceAddressLength > 3) {
      Debug.printf(" srct addr > max addr size : %d\n", sourceAddressLength);
      Clear();
    }
    return false;
  }
  else if (position == 4 + destinationAddressLength + sourceAddressLength + 2)
  {
    // Verify the header checksum
    if( !checkChecksum(position) ) {
      Debug.printf("Mismatched header checksum. Reset\n");
      Clear();
    }
    return false;
  }
  else if (position == dataLength + 1)
  {
    // Verify the data package checksum
    if( !checkChecksum(position) ) {
      Debug.printf("Mismatched frame checksum. Reset\n");
      Clear();
    }
    return false;
  }
  else if (position == dataLength + 2)
  {
    // We're done, check the stop flag and signal we're done
    if (data == START_OF_FRAME) {
      Debug.println("Done!");
      return true;
    }
    else
    {
      Debug.printf(" Stop error!\n");
      Clear();
      return false;
    }
  }
  return false;
}

void dumpHex(byte *buf, int len)
{
  char bb[2048];
  int p = 0;
  sprintf(bb, "HexDump");
  for(int n=0; n<len; n++) {
      if( (n & 0x01F) == 0 ) {
        Debug.printf("%s\n", bb);
        p = sprintf(bb, "%02X: ", n);
      }
      p += sprintf(bb+p,"%02X ", buf[n]);
  }
  if( p > 5 )
    Debug.printf("%s\n", bb);
}

int GetAddress(int addressPosition, byte* addressBuffer, int start, int length)
{
  int addressBufferPos = start;
  for (int i = addressPosition; i < position; i++)
  {
    addressBuffer[addressBufferPos++] = buffer[i];

    // LSB=1 means this was the last address byte
    if ((buffer[i] & 0x01) == 0x01)
      break;

    // See if we've reached last byte, try again when we've got more data
    else if (i == position - 1)
      return 0;
  }
  return addressBufferPos - start;
}

uint16_t GetChecksum(int checksumPosition)
{
  return (uint16_t)(buffer[checksumPosition + 1] << 8 |
    buffer[checksumPosition]);
}

bool GetUserDataBuffer(byte *&dataBuffer, int& length)
{
  if (dataLength > 0 && position - 2 == dataLength)
  {
    int headerLength = 4 + destinationAddressLength + sourceAddressLength + 2;
    // int bytesWritten = 0;
    dataBuffer = &(buffer[0 + headerLength]);
    length = dataLength - headerLength - 2;
    return true;
  }
  else {
    Debug.printf("Invalid buffer length and position. Resetting.\n");
    Clear();
    return false;
  }
}



void sendMsg(const char *topic, const char *m)
{
#define MSG_LEN 64
  char msg[MSG_LEN];
  int len = strlen(m);
  if( topic )
    snprintf(msg, MSG_LEN, "%s/%s", mqtt_msg, topic);
  else
    snprintf(msg, MSG_LEN, "%s", mqtt_msg);

  if (!client.connected())
    reconnect();
  client.publish(msg, m, true);
}
