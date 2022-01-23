#ifndef __KAMSTRUP_H__
#define __KAMSTRUP_H__

typedef struct {
  byte *obis;
  char *name;
  char *unit;
  float div;
  unsigned int *val;
} OBIS_t;

typedef struct {
  struct tm time;
  unsigned int apow[2];
  unsigned int rpow[2];
  unsigned int curr[3];
  unsigned int maxCurr[3];
  unsigned int volt[3];
  unsigned int aenerg[2];
  unsigned int renerg[2];
  unsigned int dayPower;
} Energy_t;

void clearMeter(void);
bool decodeKaifaKamstrupMeter(byte *data);
int getO16int(byte *data);
int getO32int(byte *data);


#endif//__KAMSTRUP_H__
