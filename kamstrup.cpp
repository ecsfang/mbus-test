#include <ESP8266WiFi.h>
#include "kamstrup.h"
#include "RemoteDebug.h"  //https://github.com/JoaoLopesF/RemoteDebug

extern RemoteDebug Debug;

extern Energy_t meter;

extern void sendMsg(const char *topic, const char *m);

int decodeData(byte *data);


byte rtc[6]             = { 0, 1,  1, 0, 9, 255 };
byte meterId[6]         = { 1, 1,  0, 0, 5, 255 };
byte meterType[6]       = { 1, 1, 96, 1, 1, 255 };
byte actPowerPos[6]     = { 1, 1,  1, 7, 0, 255 };
byte actPowerNeg[6]     = { 1, 1,  2, 7, 0, 255 };
byte reactPowerPos[6]   = { 1, 1,  3, 7, 0, 255 };
byte reactPowerNeg[6]   = { 1, 1,  4, 7, 0, 255 };
byte actEnergyPos[6]    = { 1, 1,  1, 8, 0, 255 };
byte actEnergyNeg[6]    = { 1, 1,  2, 8, 0, 255 };
byte reactEnergyPos[6]  = { 1, 1,  3, 8, 0, 255 };
byte reactEnergyNeg[6]  = { 1, 1,  4, 8, 0, 255 };
byte l1Current[6]       = { 1, 1, 31, 7, 0, 255 };
byte l2Current[6]       = { 1, 1, 51, 7, 0, 255 };
byte l3Current[6]       = { 1, 1, 71, 7, 0, 255 };
byte l1Voltage[6]       = { 1, 1, 32, 7, 0, 255 };
byte l2Voltage[6]       = { 1, 1, 52, 7, 0, 255 };
byte l3Voltage[6]       = { 1, 1, 72, 7, 0, 255 };

byte everyHour[6]       = { 0, 1,  1, 0, 0, 255 };

OBIS_t  obis[] = {
  { rtc,            "       RTC",   NULL,   0.0, NULL },
  { meterId,        "  Meter ID",   NULL,   0.0, NULL },
  { meterType,      "Meter Type",   NULL,   0.0, NULL },
  { actPowerPos,    "A Power+",     "W",    0.0, &meter.apow[0] },
  { actPowerNeg,    "A Power-",     "W",    0.0, &meter.apow[1] },
  { reactPowerPos,  "R Power+",     "W",    0.0, &meter.rpow[0] },
  { reactPowerNeg,  "R Power-",     "W",    0.0, &meter.rpow[1] },
  { l1Current,      "I1",           "A",  100.0, &meter.curr[0] },
  { l2Current,      "I2",           "A",  100.0, &meter.curr[1] },
  { l3Current,      "I3",           "A",  100.0, &meter.curr[2] },
  { l1Voltage,      "V1",           "V",    0.0, &meter.volt[0] },
  { l2Voltage,      "V2",           "V",    0.0, &meter.volt[1] },
  { l3Voltage,      "V3",           "V",    0.0, &meter.volt[2] },
  { actEnergyPos,   "A Energy+",    "kWh",  0.0, &meter.aenerg[0] },
  { actEnergyNeg,   "A Energy-",    "kWh",  0.0, &meter.aenerg[1] },
  { reactEnergyPos, "R Energy+",    "kWh",  0.0, &meter.renerg[0] },
  { reactEnergyNeg, "R Energy-",    "kWh",  0.0, &meter.renerg[1] },
  { NULL,           NULL,           NULL,   0.0, NULL }
};

bool isObis(byte *data, byte *ob)
{
  return memcmp(data, ob, 5) == 0;
}

void printObis(char *b, int ob)
{
  OBIS_t *op = &obis[ob-1];
  int n = sprintf(b, "%s: ", op->name);
  if( op->div > 0 )
    sprintf(b+n,"%.2f%s ", *op->val/op->div, op->unit);
  else
    sprintf(b+n,"%d%s ", *op->val, op->unit);
}

int getO16int(byte *data)
{
  return data[0] * 0x100 + data[1];
}

int getO32int(byte *data)
{
  int val = 0;
  val =  data[0] * 0x1000000;
  val += data[1] * 0x10000;
  val += data[2] * 0x100;
  val += data[3] * 0x1;
  //Debug.printf("%08X %08X\n" , val, *((int *)data));
  return val;  
}

void getObisTime(byte *data)
{
  char buffer [80];
  meter.time.tm_year = getO16int(data) - 1900;
  meter.time.tm_mon = data[2] - 1;
  meter.time.tm_mday = data[3];
  meter.time.tm_wday = data[4];
  meter.time.tm_hour = data[5];
  meter.time.tm_min = data[6];
  meter.time.tm_sec = data[7];
  strftime (buffer,80,"%Y-%m-%d - %H:%M:%S",&meter.time);
  Debug.printf("\n%s\n" , buffer);
}

bool bFirst = true;

typedef struct {
  byte h;
  byte m;
  byte s;
  unsigned int cnt;
} Cnt_t;

Cnt_t cnt = { 255, 255, 255, 0 };

bool isMidnight()
{
  static byte prevHour = 0;
  bool ret = meter.time.tm_hour == 0 && prevHour == 23;
  prevHour = meter.time.tm_hour;
  if( ret ) {
    cnt.h = cnt.m = cnt.s = 0;
    cnt.cnt = 0;
  }
  return ret;
}

void clearMeter(void)
{
  memset(&meter, 0, sizeof(Energy_t));
}

bool decodeKaifaKamstrupMeter(byte *data) {
  // First we get an untyped date time.
  int len = data[0];
  int p = 0;

  getObisTime(&data[1]);

  if( cnt.h == 255 ) {
    cnt.h = meter.time.tm_hour;
    cnt.m = meter.time.tm_min;
    cnt.s = meter.time.tm_sec;
  }
  if( isMidnight() ) {
    // Reset counter values
    for(int i=0; i<3; i++)
      meter.maxCurr[i] = 0;
    meter.dayPower = 0;
  }

  p += len + 1;
  if( data[p] != 0x02 )
    return false;
  int ns = data[p+1];
  p += 2;

  for(int s = 0; s < ns; s++) {
    Debug.printf("Msg[%d:%d] ", s+1, p);
    p += decodeData(&data[p]);
  }
  Debug.printf("\n");
  bFirst = false;

  for(int i=0; i<3; i++) {
    if( meter.curr[i] > meter.maxCurr[i] )
      meter.maxCurr[i] = meter.curr[i];
  }

  meter.dayPower += meter.apow[0];

  cnt.h = cnt.cnt / 360;
  cnt.m = (cnt.cnt / 6) - cnt.h*60;
  cnt.s = (cnt.cnt % 6) * 10;

  cnt.cnt++;

  { // Have updates to send ...
    char fBuf[128];
    int _n = sprintf(fBuf, "{");
    for(int i=0; i<3; i++)
      _n += sprintf(fBuf+_n, "\"l%d\":%.2f,\"m%d\":%.2f,", i+1, meter.curr[i]/100.0, i+1, meter.maxCurr[i]/100.0);
    _n += sprintf(fBuf+_n, "\"p\":%d, ", meter.apow[0]);
    _n += sprintf(fBuf+_n, "\"ptot\":%d, ", meter.dayPower);
    sprintf(fBuf+_n, "\"rssi\":%d}", WiFi.RSSI());
    sendMsg("current", fBuf);
    Debug.printf("Send: [%02d:%02d:%02d] %s\n", cnt.h, cnt.m, cnt.s, fBuf);
  }

  return true;
}

byte lastObis = 0;
int decodeData(byte *data)
{
  static char buf[256];
  int len = data[1];
  unsigned int val = 0;
  OBIS_t *op;
  byte n = 0;
  
  switch( data[0] ) {
    case 0x0A: // Visable string
      if( 1 /*bFirst*/ ) {
        n = sprintf(buf,"VSTRNG: ");
        if( lastObis ) {
          op = &obis[lastObis-1];
          n = sprintf(buf+n,"%s: ", op->name);
        }
        sprintf(buf+n,"%.*s ", len, &data[2]);
        Debug.printf("%s\n", buf);
      }
      lastObis = 0;
      break;
    case 0x0C: // Time information
      getObisTime(&data[1]);
      break;
    case 0x09: // Octet string
      {
        int i=0;
        lastObis = 0;
        n = sprintf(buf,"OCTST9: ");
        if( len == 6 ) {
          while( obis[i].name ) {
            if( isObis(data+2, obis[i].obis) )
              lastObis = i+1;
            i++;
          }
          if( !lastObis ) {
            // Don't count the hourly message ...
            if( isObis(data+2, everyHour) )
              cnt.cnt--;
            sprintf(buf+n,"(%d)[%d.%d.%d.%d.%d.%d] ", len, data[2], data[3], data[4], data[5], data[6], data[7]);
          }
          else
            sprintf(buf+n,"Got {%d.%d.%d.%d.%d.%d} ", len, data[2], data[3], data[4], data[5], data[6], data[7]);
        } else {
          n = sprintf(buf,"len = %d", len);
        }
        Debug.printf("%s\n", buf);
      }
      break;
    case 0x06: // Octet string
      n = sprintf(buf,"OCTST6: ");
      val = getO32int(&data[1]);
      if( lastObis ) {
        op = &obis[lastObis-1];
        *op->val = val;
        printObis(buf+n, lastObis);
      } else {
        sprintf(buf+n,"Val: %d ", val);
      }
      Debug.printf("%s\n", buf);
      len = 3;
      lastObis = 0;
      break;
    case 0x12: // 2 bytes
      n = sprintf(buf,"BYTE:   ");
      val = getO16int(&data[1]);
      if( lastObis ) {
        op = &obis[lastObis-1];
        *op->val = val;
        printObis(buf+n, lastObis);
      } else {
        sprintf(buf+n,"Val: %d ", val);
      }
      Debug.printf("%s\n", buf);
      lastObis = 0;
      len = 1;
      break;
    default:
      sprintf(buf,"<Uknwn: 0x%02X>", data[0]);
      Debug.printf("%s", buf);
  }
  return len + 2;
}
