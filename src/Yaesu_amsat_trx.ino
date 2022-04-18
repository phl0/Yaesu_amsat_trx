/* 
 * Teensy 3.2 / 4.0
 * Arduino 1.8.13 or PlatformIO
 * Author: SA2KNG
 * Display: SSD1351 oled 128x128 or 128x96
 * Control: two Yaesu FT-8x7 tranceivers in full duplex
 * GPS: generic nmea-0183
 * 
 * Supported modes:
 * - Doppler control: increase in frequency on the master results in decrease on slave, multiplied regarding to frequency
 * - Linear control: soon (tm)
 * 
 * To be implemented:
 * - Satellite configuration from SDcard, satellites.txt (norad, qrg, mode)
 * - GPS for position and time, used in the doppler calculations
 * - TLE from nasabare.txt with up to date tle's
 */

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 96    // 96 or 128
#define MOSI_PIN 11         // hardware spi
#define MISO_PIN 12         // only for sd card
#define SCLK_PIN 13         // hardware spi
#define CS_PIN   2
#define RST_PIN  3
#define DC_PIN   4
#define SDCS_PIN 5

#define NUM_SAT_CONF    16    // max number of satellites read from config

#define BLACK           0x0000
#define BLUE            0x001F
#define RED             0xF800
#define GREEN           0x07E0
#define CYAN            0x07FF
#define MAGENTA         0xF81F
#define YELLOW          0xFFE0  
#define WHITE           0xFFFF

#include <TimeLib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <Adafruit_BusIO_Register.h>
#include <Adafruit_GPS.h>
#include <SdFat.h>
#include <SPI.h>
#include <Sgp4.h>

Adafruit_SSD1351 tft = Adafruit_SSD1351(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, CS_PIN, DC_PIN, RST_PIN);
SdFs sd;
FsFile file;
HardwareSerial& r1 = Serial1;  // Radio 1: T4.0 RX 0, TX 1 / T3.2 RX 0, TX 1
HardwareSerial& r2 = Serial2;  // Radio 2: T4.0 RX 7, TX 8 / T3.2 RX 9, TX 10
HardwareSerial& gpsport = Serial3;  // GPS: T4.0 RX 15, TX 14 / T3.2 RX 7, TX 8
Adafruit_GPS GPS(&gpsport);
Sgp4 sat;

char write_freq[5] = {0,0,0,0,0x01};  // write frequency
char read_freq[5] = {0,0,0,0,0x03};   // read frequency and mode
char read_rxs[5] = {0,0,0,0,0xE7};    // read receiver status
char read_txs[5] = {0,0,0,0,0xF7};    // read transmitter status

char r1_read[10], r2_read[10];  // RX buffer from radio
byte r1_req, r2_req;            // RX bytes requested
long r1_freq, r1_oldfreq, r1_lockfreq, r2_freq, r2_oldfreq, r2_lockfreq;
byte r1_lock, r2_lock;  // locking mode, selecting master/slave
unsigned long r1_time, r2_time, poll_time, sat_time;   // millis on last command, last poll
const int intervals[2] = {1000,100};    // slow/fast polling interval
unsigned int poll_interval, timeout = 50;  // in ms
float vfo_factor;   // factor between the vfo's at locking
double gps_jdt, rtc_jdt, dist_old, sat_speed; // julian date, old sat distance
byte gps_oldsecond, disp_mode, disp_oldmode = 255;
int oldsecond;
long downlink, uplink, downlink_doppler, uplink_doppler; // deciherz, same as frequency resolution
unsigned long norad;  // norad ID

typedef enum { MODE_UNK, MODE_FM, MODE_USB, MODE_LSB } radio_mode;

typedef struct { // display name, norad, uplink_qrg, uplink_mode, downlink_qrg, downlink_mode
  char name[16];
  unsigned long norad;
  unsigned long uplink_qrg;
  unsigned long downlink_qrg;
  radio_mode uplink_mode; // 0 unknown, 1 fm, 2 usb, 3 lsb
  radio_mode downlink_mode;
} satellite_conf;
satellite_conf satellites[NUM_SAT_CONF];
byte sat_ptr = 0, sat_num = 0;

unsigned long from_bcd_be(char* c){
    unsigned long result(0);
    byte l=4;
    while (l--) {
        result = result * 100 + (*c >> 4) * 10 + (*c & 15);
        ++c;
    }
    return result;
}


char *to_bcd_be(char bcd_data[], unsigned long freq, unsigned char bcd_len){
    int i;
    unsigned char a;

    if (bcd_len&1) {
        bcd_data[bcd_len/2] &= 0x0f;
        bcd_data[bcd_len/2] |= (freq%10)<<4;
        freq /= 10;
    }
    for (i=(bcd_len/2)-1; i >= 0; i--) {
        a = freq%10;
        freq /= 10;
        a |= (freq%10)<<4;
        freq /= 10;
        bcd_data[i] = a;
    }
    return bcd_data;
}

radio_mode str2mode(char *str, size_t sze){
  if(strncmp_P(str, "fm", sze)==0) return MODE_FM;
  if(strncmp_P(str, "usb", sze)==0) return MODE_USB;
  if(strncmp_P(str, "lsb", sze)==0) return MODE_LSB;
  return MODE_UNK;
}

void updateDisplay(){
  if(disp_oldmode != disp_mode){
    disp_oldmode = disp_mode;
    tft.fillScreen(BLACK);
    tft.setTextColor(WHITE, BLACK);
  }
  if(disp_mode==0){
    tft.setCursor(0,0);
    tft.setTextSize(1);
    tft.print(F("Radio 1: RX"));
    tft.setCursor(0,SCREEN_HEIGHT/2);
    tft.print(F("Radio 2: TX"));
    tft.setCursor(0,16);
    tft.setTextSize(2);
    if(r1_freq == 1) tft.print(F("Timeout.  "));
    else tft.printf("%03li.%03li.%02li", r1_freq/100000, r1_freq/100 % 1000, r1_freq % 100);
    tft.setCursor(0,SCREEN_HEIGHT/2 + 16);
    tft.setTextSize(2);
    if(r2_freq == 1) tft.print(F("Timeout.  "));
    else tft.printf("%03li.%03li.%02li", r2_freq/100000, r2_freq/100 % 1000, r2_freq % 100);

  }else if(disp_mode==1){
    tft.setCursor(0,0);
    tft.setTextSize(1);
    tft.printf(PSTR("Sat: %s\n"), sat.satName);
    tft.printf(PSTR("Az: %.1lf\nEl: %.1lf\nSpeed: %.2lf km/s\n\n"), sat.satAz, sat.satEl, sat_speed);
    //tft.setCursor(0,40);
    tft.printf(PSTR("UL: %9li %5.0lf\n"), uplink*10, (double)-sat_speed / 29979 * uplink);
    tft.printf(PSTR("UD: %9li\n\n"), uplink_doppler*10);
    tft.printf(PSTR("DL: %9li %5.0lf\n"), downlink*10, (double)sat_speed / 29979 * downlink);
    tft.printf(PSTR("DD: %9li\n"), downlink_doppler*10);

  }else if(disp_mode==2){
    tft.setCursor(0,0);
    tft.setTextSize(1);
    tft.printf(PSTR("System time: %02d:%02d:%02d"), hour(), minute(), second());
    tft.printf(PSTR("GPS time:    %02d:%02d:%02d"), GPS.hour, GPS.minute, GPS.seconds);

  }else{
    disp_mode=0;
  }
}

time_t getTeensy3Time()
{
  return Teensy3Clock.get();
}

void processSyncMessage() {
  Serial.setTimeout(100);   // allow 100ms for time message
  time_t t = Serial.parseInt();
  if (t != 0) {
    Teensy3Clock.set(t);
    setTime(t);
    Serial.println(F("Time set via sync message."));
  }
}

void readConf(){
  char *ptr, *token, row[128];
  bool confLoaded = false;
  const char *delim = ",";
  satellite_conf tmp;
  if(sd.fatType() > 0){
    if (file.open("satellites.txt", FILE_READ)) {
      // display name, norad, uplink_qrg, uplink_mode, downlink_qrg, downlink_mode
      while(file.available()){
        file.readBytesUntil('\n', row, sizeof(row));
        if(row[0]=='#') continue;    // skip comments
        token = strtok(row, delim);
        if(token==NULL) continue;
        strncpy(tmp.name, token, sizeof(tmp.name));
        token=strtok(NULL, delim);
        if(token==NULL) continue;
        tmp.norad = strtol(token, &ptr, 10);
        if(tmp.norad < 1) continue;
        token=strtok(NULL, delim);
        if(token==NULL) continue;
        tmp.uplink_qrg=strtol(token, &ptr, 10);
        if(tmp.uplink_qrg < 1) continue;
        token=strtok(NULL, delim);
        if(token==NULL) continue;
        tmp.uplink_mode=str2mode(token, sizeof(token));
        token=strtok(NULL, delim);
        if(token==NULL) continue;
        tmp.downlink_qrg=strtol(token, &ptr, 10);
        if(tmp.downlink_qrg < 1) continue;
        token=strtok(NULL, delim);
        if(token==NULL) continue;
        tmp.downlink_mode=str2mode(token, sizeof(token));
        memcpy(&satellites[sat_num], &tmp, sizeof(tmp));
        if(sat_num + 1 >= NUM_SAT_CONF) break;
        sat_num++;
      }
      file.close();
      confLoaded = true;
    }else{
      Serial.println(F("cannot find satellites.txt"));
    }
  }else{
    Serial.println(F("uknown filesystem on SD"));
  }
  if(!confLoaded){
    Serial.println(F("Failed to load configuration, defaults used."));
    norad = 39444;
    downlink = 14596000;
    uplink = 43514000;
  }else{
    //norad = satellites[sat_ptr].norad;
    //downlink = satellites[sat_ptr].downlink_qrg/10;
    //uplink = satellites[sat_ptr].uplink_qrg/10;
    loadSat();  // proper time/pos probably not set yet...
    Serial.printf(PSTR("Satellites loaded: %d\n"), sat_num);
  }
}

void loadSat(void){
  //Serial.printf(PSTR("Satellite selected: %d\n"), sat_ptr);
  norad = satellites[sat_ptr].norad;
  downlink = satellites[sat_ptr].downlink_qrg/10;
  uplink = satellites[sat_ptr].uplink_qrg/10;
  readTLE(norad);
  findPass();
}

void findPass(void){
  passinfo overpass;
  int yr, mon, dy, hr, mn; double sec;
  jday(year(), month(), day(), hour(), minute(), second(), 0, false, rtc_jdt);
  sat.initpredpoint(rtc_jdt, 0.0);
  if(sat.nextpass(&overpass,20)==1){
    invjday(overpass.jdstart ,0 ,true , yr, mon, dy, hr, mn, sec);
    Serial.printf(PSTR("Overpass %4d-%02d-%02d\n"), yr, mon, dy);
    Serial.printf(PSTR("  Start: az=%lf at %02d:%02d:%02lf\n"), overpass.azstart, hr, mn, sec);
    invjday(overpass.jdmax ,0 ,true , yr, mon, dy, hr, mn, sec);
    Serial.printf(PSTR("  Max: elev=%lf at %02d:%02d:%02lf\n"), overpass.maxelevation, hr, mn, sec);
    invjday(overpass.jdstop ,0 ,true , yr, mon, dy, hr, mn, sec);
    Serial.printf(PSTR("  Stop: az=%lf at %02d:%02d:%02lf\n"), overpass.azstop, hr, mn, sec);
  }else{
    Serial.print(F("Could not find satellite pass.\n"));
  }
}

void readTLE(int noradID){
  char row1[128], row2[128], row3[128], nsearch[10];
  bool tleloaded = false;
  sprintf(nsearch, "2 %05d ", noradID);   // search for the last row
  if(sd.fatType() > 0){
    if (file.open("nasabare.txt", FILE_READ)) {
      while(file.available()){
        strncpy(row1, row2, sizeof(row1));
        strncpy(row2, row3, sizeof(row2));
        file.readBytesUntil('\n', row3, sizeof(row3));
        if(strncmp(row3, nsearch, 8)==0){
          tleloaded = true;
          break;
        }
      }
      file.close();
    }else{
      Serial.println(F("cannot find nasabare.txt"));
    }
  }
  if(!tleloaded){ // failed to load, use hardcoded
    Serial.println(F("Failed to find satellite, using hardcoded."));
    strncpy_P(row1, PSTR("AO-73 (hardcoded)"), sizeof(row1)); // TLE date 2022-04-13
    strncpy_P(row2, PSTR("1 39444U 13066AE  22097.48225341  .00003217  00000-0  39123-3 0  9993"), sizeof(row2));
    strncpy_P(row3, PSTR("2 39444  97.6190  74.6247 0055298 265.9560  93.5332 14.83068053451003"), sizeof(row3));
  }
  sat.init(row1, row2, row3);     //initialize satellite parameters
  //Serial.printf(PSTR("satname: %s\ntle1: %s\ntle2: %s\n"), satname, tle_line1, tle_line2);
  Serial.printf(PSTR("Sat init: %s\n"), sat.satName);
}

void setup() {
  Serial.begin(115200);   // USB Serial monitor
  r1.begin(9600);         // Radio 1
  r2.begin(9600);         // Radio 2
  setSyncProvider(getTeensy3Time);
  while (!Serial && millis() < 2000);   // wait 2s for serial monitor

  Serial.println(F("Yaesu amsat trx with dual FT-817/818/857. By: Daniel SA2KNG."));
  if (timeStatus()!= timeSet) {
    Serial.println(F("Unable to sync with the RTC"));
  }

  GPS.begin(9600);        // make sure your module settings matches this, common 4800 or 9600

  tft.begin();
  tft.setRotation(2);     // rotate display 180 deg if necessary
  updateDisplay();        // draw display
  
  if (!sd.begin(SdSpiConfig(SDCS_PIN, SHARED_SPI, 20))) {
    sd.initErrorHalt(&Serial);
  }
  //sd.ls(LS_DATE | LS_SIZE); // show files on card
  readConf();             // read config
  readTLE(norad);         // search and load TLE from file

  poll_interval = intervals[0];
  poll_time = millis();
}

void loop() {   // non blocking loop, no delays or blocking calls

  GPS.read();
  if (GPS.newNMEAreceived()) {  // parse nmea
    GPS.parse(GPS.lastNMEA());
  }

  if(second() != oldsecond){    // use system RTC
    oldsecond = second();
    jday(year(), month(), day(), hour(), minute(), second(), 0, false, rtc_jdt);
    if(GPS.fix && GPS.secondsSinceTime() < 3.0){
      jday((int)2000 + GPS.year, GPS.month, GPS.day, GPS.hour, GPS.minute, GPS.seconds, 0, false, gps_jdt);
      if(rtc_jdt > gps_jdt + 0.00005 || rtc_jdt < gps_jdt - 0.00005){  // ~4s tolerance for setting rtc
        Serial.println(F("Setting system time from gps."));
        setTime(GPS.hour, GPS.minute, GPS.seconds, GPS.day, GPS.month, GPS.year);
        jday(year(), month(), day(), hour(), minute(), second(), 0, false, rtc_jdt);
      }
      sat.site(GPS.latitudeDegrees, GPS.longitudeDegrees, GPS.altitude);
    }
    rtc_jdt -= (double) 10 / 86400;   // calculate speed over positions separated by 10s
    sat.findsat(rtc_jdt);
    dist_old = sat.satDist;
    rtc_jdt += (double) 10 / 86400;
    sat.findsat(rtc_jdt);
    sat_speed = (dist_old - sat.satDist) / 10;
    uplink_doppler = uplink - sat_speed / 29979 * uplink / 10;  // deciherz
    downlink_doppler = downlink + sat_speed / 29979 * downlink / 10;
    Serial.printf(PSTR("Sat %s: Az %.0lf, El %.0lf, Speed %.2lf km/s\n"), sat.satName, sat.satAz, sat.satEl, sat_speed);
    if(disp_mode==1 || disp_mode==2) updateDisplay();
  }

  if(Serial.available()){     // USB serial as manager, taking single byte commands for simplicity
    char c = Serial.read();
    if(c=='1'){   // show radio
      disp_mode=0;
    }else if(c=='2'){   // show satellite
      disp_mode=1;
    }else if(c=='3'){   // show time
      disp_mode=2;

    }else if(c=='r'){
      Serial.println("read_rxs 1");
      r1.write(read_rxs, sizeof(read_rxs));
      r1_time = millis();
      r1_req=1;
    }else if(c=='t'){
      Serial.println("read_txs 1");
      r1.write(read_txs, sizeof(read_txs));
      r1_time = millis();
      r1_req=1;

    }else if(c=='9'){   // start polling
      poll_time = millis();
    }else if(c=='0'){   // stop polling
      poll_time = 0;

    }else if(c=='l'){ // lock vfo's
      poll_interval = intervals[1];
      r1_lock = 1;    // master
      r2_lock = 0;    // slave
    }else if(c=='L'){ // lock vfo's
      poll_interval = intervals[1];
      r1_lock = 0;    // slave
      r2_lock = 1;    // master
    }else if(c=='u'){ // unlock vfo's
      poll_interval = intervals[0];
      poll_time = millis();
      r1_lock = 0;    // independent
      r2_lock = 0;
    }else if(c=='d'){ // doppler soft controlled, for linears
      poll_time = 0;
      r1_lock = 3;
      r2_lock = 3;
    }else if(c=='D'){ // doppler forced, for fm
      poll_time = 0;
      r1_lock = 4;
      r2_lock = 4;

    }else if(c=='n'){ // next satellite from conf
      sat_ptr++;
      if(sat_ptr >= sat_num) sat_ptr = 0;
      loadSat();
    }else if(c=='N'){ // previour satellite
      if(sat_ptr == 0) sat_ptr = sat_num;
      else sat_ptr--;
      loadSat();

    }else if(c=='g'){ // show gps
      Serial.printf(PSTR("Time: %2d:%2d:%2d\n"), GPS.hour, GPS.minute, GPS.seconds);
      Serial.printf(PSTR("Latitude: %.3f, Longitude: %.3f\n"), GPS.latitudeDegrees, GPS.longitudeDegrees);

    }else if(c=='T'){ // temporarily enter timesync mode, 100ms blocking!
      processSyncMessage();
    }
  }

  // parse responses from radio 1
  if(r1_req > 0 && r1.available() == r1_req){           // does the response match the request ?
    for(int i=0;i<r1_req;i++) r1_read[i] = r1.read();
    if(r1_req == 5){                                    // frequency response
      r1_freq = from_bcd_be(r1_read);                   // convert bcd array to long
      //Serial.print("r1_freq: ");
      //Serial.println(r1_freq);
    }else{                                              // single byte response
      Serial.println(r1_read[0], HEX);
    }
    r1_req=0;                                           // clear request length
    if(disp_mode==0) updateDisplay();
  }else if(r1_req == 0 && r1.available()){              // flush unrequested data
    r1.read();
  }
  if(r1_req > 0 && millis() > r1_time + timeout){       // request timeout
    r1_freq = 1;
    r1_req = 0;
    if(disp_mode==0) updateDisplay();
  }

  // parse responses from radio 2
  if(r2_req > 0 && r2.available() == r2_req){           // does the response match the request ?
    for(int i=0;i<r2_req;i++) r2_read[i] = r2.read();
    if(r2_req == 5){                                    // frequency response
      r2_freq = from_bcd_be(r2_read);                   // convert bcd array to long
      //Serial.print("r2_freq: ");
      //Serial.println(r2_freq);
    }else{                                              // single byte response
      Serial.println(r2_read[0], HEX);
    }
    r2_req=0;                                           // clear request length
    if(disp_mode==0) updateDisplay();
  }else if(r2_req == 0 && r2.available()){              // flush unrequested
    r2.read();
  }
  if(r2_req > 0 && millis() > r2_time + timeout){       // request timeout
    r2_freq = 1;
    r2_req = 0;
    if(disp_mode==0) updateDisplay();
  }

  // locked vfo with r1 as master
  if(r1_lock == 1){
    r1_lockfreq = r1_oldfreq = r1_freq;   // store the locked frequencies
    r2_lockfreq = r2_freq;
    r1_lock = 2;
    r2_lock = 0;
    vfo_factor = (float) r2_freq / r1_freq;
    Serial.println("Locking with radio 1 master");
    Serial.print("vfo_factor: ");
    Serial.println(vfo_factor);
  }else if(r1_lock == 2){
    if(r1_freq != r1_oldfreq){
      r2_freq = r2_lockfreq - vfo_factor * (r1_freq - r1_lockfreq);
      r1_oldfreq = r1_freq;
      Serial.print("r1_freq: ");
      Serial.println(r1_freq);
      Serial.print("r2_freq: ");
      Serial.println(r2_freq);
      to_bcd_be(write_freq, r2_freq, 8);
      r2.write(write_freq, sizeof(write_freq));
      if(disp_mode==0) updateDisplay();
    }
  }else if(r1_lock == 3){ // doppler controlled
    if(poll_time != 0 && millis() - poll_time >= poll_interval){
      poll_time = millis();
      //r1_freq = downlink + sat_speed / 29979 * uplink
      //to_bcd_be(write_freq, r1_freq, 8);
      //r1.write(write_freq, sizeof(write_freq));
      //to_bcd_be(write_freq, r2_freq, 8);
      //r2.write(write_freq, sizeof(write_freq));
    }
  }else if(r1_lock == 4){ // doppler forced
    if(millis() - poll_time >= poll_interval){
      //poll_time = millis(); // handled below
      r1_freq = downlink + sat_speed / 29979 * downlink / 10;
      to_bcd_be(write_freq, r1_freq, 8);
      r1.write(write_freq, sizeof(write_freq));
      //Serial.println(r1_freq);
    }
  }else{
    r1_lock = 0;
  }

  if(r2_lock == 1){
    r2_lockfreq = r2_oldfreq = r2_freq;   // store the locked frequencies
    r1_lockfreq = r1_freq;
    r2_lock = 2;
    r1_lock = 0;
    vfo_factor = (float) r1_freq / r2_freq;
    Serial.println("Locking with radio 2 master");
    Serial.print("vfo_factor: ");
    Serial.println(vfo_factor);
  }else if(r2_lock == 2){
    if(r2_freq != r2_oldfreq){
      r1_freq = r1_lockfreq - vfo_factor * (r2_freq - r2_lockfreq);
      r2_oldfreq = r2_freq;
      Serial.print("r1_freq: ");
      Serial.println(r1_freq);
      Serial.print("r2_freq: ");
      Serial.println(r2_freq);
      to_bcd_be(write_freq, r1_freq, 8);
      r2.write(write_freq, sizeof(write_freq));
      if(disp_mode==0) updateDisplay();
    }
  }else if(r2_lock == 3){
    // handled in r1 above
  }else if(r2_lock == 4){ // doppler forced
    if(millis() - poll_time >= poll_interval){
      poll_time = millis();
      r2_freq = uplink - sat_speed / 29979 * uplink / 10;
      to_bcd_be(write_freq, r2_freq, 8);
      r2.write(write_freq, sizeof(write_freq));
      //Serial.println(r2_freq);
    }
  }else{
    r2_lock = 0;
  }

  // auto polling of radios
  if(poll_time != 0 && millis() - poll_time >= poll_interval){
    poll_time = r1_time = r2_time = millis();
    //Serial.println("polling radios");
    r1.write(read_freq, sizeof(read_freq));             // TODO: reduce poll interval when timed out, hard to turn on/off radio otherwise
    r1_req=5;
    r2.write(read_freq, sizeof(read_freq));
    r2_req=5;
  }
}

