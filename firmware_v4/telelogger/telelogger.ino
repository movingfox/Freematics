/******************************************************************************
* Vehicle Telematics Data Logger Sketch for Freematics ONE
* Distributed under BSD license
* Visit http://freematics.com/products/freematics-one for more information
* Developed by Stanley Huang <support@freematics.com.au>
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
******************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <FreematicsONE.h>
#include "config.h"
#include "datalogger.h"

// logger states
#define STATE_SD_READY 0x1
#define STATE_OBD_READY 0x2
#define STATE_GPS_READY 0x4
#define STATE_MEMS_READY 0x8
#define STATE_SLEEPING 0x20
#define STATE_CONNECTED 0x40

static uint16_t lastUTC = 0;
static uint8_t lastGPSDay = 0;
static uint32_t nextConnTime = 0;
static uint16_t connCount = 0;
static char vin[20] = {0};

typedef enum {
    GPRS_DISABLED = 0,
    GPRS_IDLE,
    GPRS_HTTP_CONNECTING,
    GPRS_HTTP_RECEIVING,
    GPRS_HTTP_ERROR,
} GPRS_STATES;

typedef enum {
  HTTP_GET = 0,
  HTTP_POST,
} HTTP_METHOD;

typedef struct {
  float lat;
  float lng;
  uint8_t year; /* year past 2000, e.g. 15 for 2015 */
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} GSM_LOCATION;

class COBDGSM : public COBDSPI {
public:
    COBDGSM():gprsState(GPRS_DISABLED),connErrors(0) { buffer[0] = 0; }
    void toggleGSM()
    {
        setTarget(TARGET_OBD);
        sendCommand("ATGSMPWR\r", buffer, sizeof(buffer));
    }
    bool initGSM()
    {
      // init xBee module serial communication
      xbBegin();
      // discard any stale data
      xbPurge();
      for (;;) {
        // try turning on GSM
        toggleGSM();
        delay(2000);
        if (sendGSMCommand("ATE0\r") != 0) {
          break;
        }
      }
      //sendGSMCommand("ATE0\r");
    }
    bool setupGPRS(const char* apn)
    {
      while (sendGSMCommand("AT+CREG?\r", 5000, "+CREG: 0,") == 0) {
        Serial.print('.'); 
      }
      sendGSMCommand("AT+CGATT?\r");
      sendGSMCommand("AT+SAPBR=3,1,\"Contype\",\"GPRS\"\r");
      sprintf_P(buffer, PSTR("AT+SAPBR=3,1,\"APN\",\"%s\"\r"), apn);
      sendGSMCommand(buffer, 15000);
      do {
        sendGSMCommand("AT+SAPBR=1,1\r", 5000);
        sendGSMCommand("AT+SAPBR=2,1\r", 5000);
      } while (strstr_P(buffer, PSTR("0.0.0.0")) || strstr_P(buffer, PSTR("ERROR")));
      //Serial.println(buffer);
      return true;
    }
    int getSignal()
    {
        if (sendGSMCommand("AT+CSQ\r", 500)) {
            char *p = strchr(buffer, ':');
            if (p) {
              p += 2;
              int db = atoi(p) * 10;
              p = strchr(p, '.');
              if (p) db += *(p + 1) - '0';
              return db;
            }
        }
        return -1;
    }
    bool getOperatorName()
    {
        // display operator name
        if (sendGSMCommand("AT+COPS?\r") == 1) {
            char *p = strstr(buffer, ",\"");
            if (p) {
                p += 2;
                char *s = strchr(p, '\"');
                if (s) *s = 0;
                strcpy(buffer, p);
                return true;
            }
        }
        return false;
    }
    void httpUninit()
    {
      sendGSMCommand("AT+HTTPTERM\r");
    }
    bool httpInit()
    {
      if (!sendGSMCommand("AT+HTTPINIT\r", 3000) || !sendGSMCommand("AT+HTTPPARA=\"CID\",1\r", 3000)) {
        gprsState = GPRS_DISABLED;
        return false;
      }
      gprsState = GPRS_IDLE;
      return true;
    }
    void httpConnect(HTTP_METHOD method)
    {
        // 0 for GET, 1 for POST
        char cmd[17];
        sprintf_P(cmd, PSTR("AT+HTTPACTION=%c\r"), '0' + method);
        setTarget(TARGET_BEE);
        write(cmd);
        gprsState = GPRS_HTTP_RECEIVING;
        bytesRecv = 0;
        checkTimer = millis();
    }
    bool httpIsConnected()
    {
        // may check for "ACTION: 0" for GET and "ACTION: 1" for POST
        byte ret = checkbuffer("ACTION:", MAX_CONN_TIME);
        if (ret == 1) {
          // success
          connErrors = 0;
          return true;
        } else if (ret == 2) {
          // timeout
          gprsState = GPRS_HTTP_ERROR;
          connErrors++;
        }
        return false;
    }
    bool httpRead()
    {
        if (sendGSMCommand("AT+HTTPREAD\r", MAX_CONN_TIME) && strstr(buffer, "+HTTPREAD:")) {
          gprsState = GPRS_IDLE;
          return true;
        } else {
          Serial.println("READ ERROR");
          gprsState = GPRS_HTTP_ERROR;
          return false;
        }
    }
    bool getLocation(GSM_LOCATION* loc)
    {
      if (sendGSMCommand("AT+CIPGSMLOC=1,1\r", 3000)) do {
        char *p;
        if (!(p = strchr(buffer, ':'))) break;
        if (!(p = strchr(p, ','))) break;
        loc->lng = atof(++p);
        if (!(p = strchr(p, ','))) break;
        loc->lat = atof(++p);
        if (!(p = strchr(p, ','))) break;
        loc->year = atoi(++p) - 2000;
        if (!(p = strchr(p, '/'))) break;
        loc->month = atoi(++p);
        if (!(p = strchr(p, '/'))) break;
        loc->day = atoi(++p);
        if (!(p = strchr(p, ','))) break;
        loc->hour = atoi(++p);
        if (!(p = strchr(p, ':'))) break;
        loc->minute = atoi(++p);
        if (!(p = strchr(p, ':'))) break;
        loc->second = atoi(++p);
        return true;
      } while(0);
      return false;
    }
    byte checkbuffer(const char* expected, unsigned int timeout = 2000)
    {
      byte ret = xbReceive(buffer, sizeof(buffer), 0, expected) != 0;
      if (ret == 0) {
        // timeout
        return (millis() - checkTimer < timeout) ? 0 : 2;
      } else {
        return ret;
      }
    }
    bool sendGSMCommand(const char* cmd, unsigned int timeout = 2000, const char* expected = "OK")
    {
      xbWrite(cmd);
      delay(10);
      return xbReceive(buffer, sizeof(buffer), timeout, expected) != 0;
    }
    bool setPostPayload(const char* payload, int bytes)
    {
        // set HTTP POST payload data
        char cmd[24];
        sprintf_P(cmd, PSTR("AT+HTTPDATA=%d,1000\r"), bytes);
        if (!sendGSMCommand(cmd, 1000, "DOWNLOAD")) {
          return false;
        }
        // send cached data
        return sendGSMCommand(payload, 1000);
    }
    char buffer[128];
    byte bytesRecv;
    uint32_t checkTimer;
    byte gprsState;
    byte connErrors;
};

class CTeleLogger : public COBDGSM, public CDataLogger
#if USE_MPU6050
,public CMPU6050
#elif USE_MPU9250
,public CMPU9250
#endif
{
public:
    CTeleLogger():state(0),channel(0) {}
    void setup()
    {
        delay(500);
        
        // initialize hardware serial (for USB or BLE)
        Serial.begin(115200);

        // this will init SPI communication
        begin();

        // initialize OBD communication
        Serial.print("#OBD..");
        for (uint32_t t = millis(); millis() - t < OBD_CONN_TIMEOUT; ) {
            Serial.print('.');
            if (init()) {
              state |= STATE_OBD_READY;
              break;              
            }
        }
        if (state & STATE_OBD_READY) {
          Serial.print("VER ");
          Serial.println(version);
        } else {
          Serial.println("NO"); 
        }

        // retrieve VIN
        if ((state & STATE_OBD_READY) && getVIN(buffer, sizeof(buffer))) {
          snprintf_P(vin, sizeof(vin), PSTR("%s"), buffer);
          Serial.print("#VIN:");
          Serial.println(vin);
        }

#if USE_MPU6050 || USE_MPU9250
        // start I2C communication 
        Wire.begin();
        // initialize MPU-6050
        Serial.print("#MEMS...");
        if (memsInit()) {
          state |= STATE_MEMS_READY;
          Serial.println("OK");
        } else {
          Serial.println("NO");
        }
#endif

#if USE_GPS
        // start serial communication with GPS receive
        Serial.print("#GPS...");
        if (initGPS(GPS_SERIAL_BAUDRATE)) {
          state |= STATE_GPS_READY;
          Serial.println("OK");
        } else {
          Serial.println("NO");
        }
#endif

        // initialize SIM800L xBee module (if present)
        Serial.print("#GSM...");
        if (initGSM()) {
            Serial.println("OK");
        } else {
            Serial.println(buffer);
        }

        Serial.print("#GPRS(APN:");
        Serial.print(APN);
        Serial.print(")...");
        if (setupGPRS(APN)) {
            Serial.println("OK");
        } else {
            Serial.println(buffer);
        }
        
        // init HTTP
        Serial.print("#HTTP...");
        while (!httpInit()) {
          Serial.print('.');
          httpUninit();
          delay(1000);
        }
        Serial.println("OK");

        // sign in server, will block if not successful
        signInOut(0);
        state |= STATE_CONNECTED;
        
        Serial.println();
        delay(1000);
    }
    void signInOut(byte action)
    {
      int signal;
      Serial.print("#SIGNAL:");
      signal = getSignal();
      Serial.println(signal);
      gprsState = GPRS_IDLE;
      for (;;) {
        char *p = buffer;
        p += sprintf_P(buffer, PSTR("AT+HTTPPARA=\"URL\",\"%s/push?"), HOST_URL);
        if (action == 0) {
          Serial.print("#SERVER:"); 
          sprintf_P(p, PSTR("CSQ=%d&VIN=%s\"\r"), signal, vin);
        } else {
          sprintf_P(p, PSTR("id=%d&OFF=1\"\r"), channel);
        }
        if (!sendGSMCommand(buffer)) {
          Serial.println(buffer);
          continue;
        }
        httpConnect(HTTP_GET);
        do {
          delay(500);
          Serial.print('.');
        } while (!httpIsConnected());
        if (action != 0) return;
        if (gprsState != GPRS_HTTP_ERROR && httpRead()) {
          char *p = strstr(buffer, "CH:");
          if (p) {
            int m = atoi(p + 3);
            if (m > 0) {
              channel = m;
              Serial.print(m);
              state |= STATE_CONNECTED;
              break;
            }
          }            
        }
        Serial.println("Error");
        Serial.println(buffer);
      }
    }
    void loop()
    {
        if (state & STATE_GPS_READY) {
#if USE_GPS
          processGPS();
#endif
#if USE_GSM_LOCATION
        } else {
          if (gprsState == GPRS_IDLE) {
            // get GSM location if GPS not present
            if (getLocation(&loc)) {
              //Serial.print(buffer);
            }
          }
          if (cacheBytes == 0) processGSMLocation();
#endif
        }

#if USE_MPU6050 || USE_MPU9250
        if (state & STATE_MEMS_READY) {
            processMEMS();  
        }
#endif

        // log battery voltage (from voltmeter), data in 0.01v
        int v = getVoltage() * 100;
        dataTime = millis();
        logData(PID_BATTERY_VOLTAGE, v);

        if (state & STATE_OBD_READY) {
          processOBD();
          if (gprsState == GPRS_IDLE && errors > 10) {
              reconnect();
          }
        }

        if (millis() > nextConnTime) {
          processGPRS();
        }
        if (connErrors >= MAX_CONN_ERRORS) {
          // reset GPRS 
          Serial.print(connErrors);
          Serial.println("Reset GPRS...");
          initGSM();
          setupGPRS(APN);
          if (httpInit()) {
            Serial.println("OK"); 
          } else {
            Serial.println(buffer); 
          }
          connErrors = 0;
        }
    }
private:
    void processGPRS()
    {
        switch (gprsState) {
        case GPRS_IDLE:
            if (state & STATE_CONNECTED) {
                // generate URL
                sprintf_P(buffer, PSTR("AT+HTTPPARA=\"URL\",\"%s/post?id=%u\"\r"), HOST_URL, channel);
                if (!sendGSMCommand(buffer)) {
                  break;
                }
                // replacing last space with
                cache[cacheBytes - 1] = '\r';
                if (setPostPayload(cache, cacheBytes - 1)) {
                  // success
                  Serial.print(cacheBytes - 1);
                  Serial.println(" bytes");
                  // output payload data to serial
                  Serial.println(cache);               
                  gprsState = GPRS_HTTP_CONNECTING;
                  purgeCache();
                } else {
                  Serial.println(buffer);
                  gprsState = GPRS_HTTP_ERROR;
                }
            }
            break;        
        case GPRS_HTTP_CONNECTING:
            Serial.print("Sending #");
            Serial.print(++connCount);
            Serial.print("...");
            httpConnect(HTTP_POST);
            nextConnTime = millis() + 2000;
            break;
        case GPRS_HTTP_RECEIVING:
            if (httpIsConnected()) {
                if (httpRead()) {
                  // success
                  Serial.println("OK");
                  //Serial.println(buffer);
                  break;
                }
            }
            nextConnTime = millis() + 200; 
            break;
        case GPRS_HTTP_ERROR:
            Serial.println("HTTP error");
            Serial.println(buffer);
            connCount = 0;
            xbPurge();
            httpUninit();
            delay(500);
            httpInit();
            gprsState = GPRS_IDLE;
            nextConnTime = millis() + 500;
            break;
        }
    }
    void processOBD()
    {
        // poll OBD-II PIDs
        static const byte pids[]= {PID_RPM, PID_SPEED, PID_ENGINE_LOAD, PID_THROTTLE};
        int values[sizeof(pids)] = {0};
        // read multiple OBD-II PIDs
        byte results = readPID(pids, sizeof(pids), values);
        dataTime = millis();
        if (results == sizeof(pids)) {
          for (byte n = 0; n < sizeof(pids); n++) {
            logData(0x100 | pids[n], values[n]);
          }
        }
        static byte index2 = 0;
        static const byte pidTier2[] = {PID_INTAKE_TEMP, PID_COOLANT_TEMP};
        byte pid = pidTier2[index2];
        int value;
        // read a single OBD-II PID
        if (readPID(pid, value)) {
          logData(0x100 | pid, value);
        }
        index2 = (index2 + 1) % sizeof(pidTier2);
    }
#if USE_MPU6050 || USE_MPU9250
    void processMEMS()
    {
        int acc[3];
        //int gyr[3];
        //int mag[3];
        int temp; // device temperature (in 0.1 celcius degree)
        if (memsRead(acc, 0, 0, &temp)) {
          dataTime = millis();
          logData(PID_ACC, acc[0] / ACC_DATA_RATIO, acc[1] / ACC_DATA_RATIO, acc[2] / ACC_DATA_RATIO);
          //logData(PID_GYRO, gyr[0] / GYRO_DATA_RATIO, gyr[1] / GYRO_DATA_RATIO, gyr[2] / GYRO_DATA_RATIO);
          //logData(PID_COMPASS, mag[0] / COMPASS_DATA_RATIO, mag[1] / COMPASS_DATA_RATIO, mag[2] / COMPASS_DATA_RATIO);
          logData(PID_MEMS_TEMP, temp);
        }
    }
#endif
    void processGPS()
    {
        GPS_DATA gd = {0};
        // read parsed GPS data
        if (getGPSData(&gd)) {
            if (lastUTC != (uint16_t)gd.time) {
              dataTime = millis();
              byte day = gd.date / 10000;
              logData(PID_GPS_TIME, gd.time);
              if (lastGPSDay != day) {
                logData(PID_GPS_DATE, gd.date);
                lastGPSDay = day;
              }
              logCoordinate(PID_GPS_LATITUDE, gd.lat);
              logCoordinate(PID_GPS_LONGITUDE, gd.lng);
              logData(PID_GPS_ALTITUDE, gd.alt);
              logData(PID_GPS_SPEED, gd.speed);
              //logData(PID_GPS_SAT_COUNT, gd.sat);
              lastUTC = (uint16_t)gd.time;
            }
            //Serial.print("#UTC:"); 
            //Serial.println(gd.time);
        } else {
          Serial.println("GPS error");
          delay(200);
        }
    }
    void processGSMLocation()
    {
        uint32_t t = (uint32_t)loc.hour * 1000000 + (uint32_t)loc.minute * 10000 + (uint32_t)loc.second * 100;
        if (lastUTC != (uint16_t)t) {
          dataTime = millis();
          logData(PID_GPS_TIME, t);
          if (lastGPSDay != loc.day) {
            logData(PID_GPS_DATE, (uint32_t)loc.day * 10000 + (uint32_t)loc.month * 100 + loc.year);
            lastGPSDay = loc.day;
          }
          logCoordinate(PID_GPS_LATITUDE, loc.lat * 1000000);
          logCoordinate(PID_GPS_LONGITUDE, loc.lng * 1000000);
          lastUTC = (uint16_t)t;
        }
    }
    void reconnect()
    {
        if (init()) {
          // reconnected
          return; 
        }
        Serial.println("Sleeping");
        signInOut(1); // sign out
        toggleGSM(); // turn off GSM power
#if USE_GPS
        initGPS(0); // turn off GPS power
#endif
        state &= ~(STATE_OBD_READY | STATE_GPS_READY | STATE_MEMS_READY);
        state |= STATE_SLEEPING;
        // regularly check if we can get any OBD data
        for (;;) {
            int value;
            Serial.print('.');
            if (readPID(PID_SPEED, value)) {
              // a successful readout
              break;
            }
            enterLowPowerMode();
            // deep sleep for 8 seconds
            sleep(8);
            leaveLowPowerMode();
        }
        // we are able to get OBD data again
        // reset device
        void(* resetFunc) (void) = 0; //declare reset function at address 0
        resetFunc();
    }
    byte state;
    byte channel;
    GSM_LOCATION loc;
};

CTeleLogger logger;

void setup()
{
    logger.initSender();
    logger.setup();
}

void loop()
{
    logger.loop();
}
